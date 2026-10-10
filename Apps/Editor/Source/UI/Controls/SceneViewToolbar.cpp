#include "UI/Controls/SceneViewToolbar.h"
#include "SceneViewController.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ScenePostFxMenu.h"
#include "UI/ContextMenuLabels.h"
#include "UI/EditorIcons.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Controls/Mount.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/Registration/ElementRegistration.h"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{
    // The scene-view toolbar's draggable groups and the preference each one's order
    // persists under. One definition: SetupDragDrop and OnPostLayout both pass it.
    const std::vector<Editor::ToolbarDragDrop::ContainerConfig>& DragDropContainers()
    {
        static const std::vector<Editor::ToolbarDragDrop::ContainerConfig> kConfig = {
            {"sceneview-toolbar-right", "ui.sceneview.toolbar.order"},
            {"inline-tool-buttons", "ui.sceneview.toolbar.inline.order"},
        };
        return kConfig;
    }

    constexpr float kSceneSnapSizePresets[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f};

    // The four scene viewports, in ViewportSlot order (Perspective, Top, Front, Side) —
    // SceneViewSettings addresses them by plain index.
    constexpr const char* kRotationGizmoSplitLabels[] = {"Perspective", "Top Y", "Front Z",
                                                         "Side X"};
} // namespace

using Editor::ContextMenuLabels::CheckedFlag;

SceneViewToolbar::SceneViewToolbar()
{
    // Default class so panel styles can target it even if layout forgets.
    AddClass("sceneview-toolbar");
}

void SceneViewToolbar::SetSceneController(SceneViewController* controller)
{
    m_Controller = controller;
    // Every button handler reaches the controller through `this` at click time, so a
    // controller swap re-binds nothing — only the icon-active classes that mirror
    // controller and settings state have to be re-read. Wiring stays one-shot per
    // button instance (WireControls / AcquireUnwiredButton).
    //
    // The scene view pushes its controller here every frame, and the refresh below is
    // what keeps the icons honest for settings mutated with no push of their own —
    // SettingsPanel's "Exact Pick Mode" and "Show Rotation Gizmo" toggles. Hence a
    // refresh rather than an early-out on an unchanged controller; it must stay free
    // of handler churn either way.
    RefreshButtonStates();
}

void SceneViewToolbar::SetupDragDrop(UIElement* rootEl, Editor::ToolbarDragDrop* dragDrop)
{
    m_DragRootEl = rootEl;
    m_DragDrop   = dragDrop;

    if (m_DragDrop && m_DragRootEl)
    {
        m_DragDrop->Setup(m_DragRootEl, DragDropContainers(), this);
        m_DragDrop->LoadButtonOrder(this, DragDropContainers());
    }
}

void SceneViewToolbar::OnPostLayout()
{
    WireControls();

    // Re-run Setup after each layout so newly parsed buttons get drag handlers.
    // ToolbarDragDrop guards against duplicate registration via the
    // "toolbar-dragdrop-initialized" class, so this is safe to call repeatedly.
    if (m_DragDrop && m_DragRootEl)
        m_DragDrop->Setup(m_DragRootEl, DragDropContainers(), this);
}

void SceneViewToolbar::UpdateGizmoButtonState()
{
    if (auto* gizmoBtn = dynamic_cast<Button*>(FindById("GizmoToggle")))
    {
        if (m_Controller && m_Controller->AreGizmosVisible())
        {
            gizmoBtn->AddClass("icon-active");
        }
        else
        {
            gizmoBtn->RemoveClass("icon-active");
        }
    }
}

void SceneViewToolbar::UpdateGridButtonState()
{
    if (auto* gridBtn = dynamic_cast<Button*>(FindById("GridToggle")))
    {
        if (m_Controller && m_Controller->IsGridVisible())
            gridBtn->AddClass("icon-active");
        else
            gridBtn->RemoveClass("icon-active");
    }
}

void SceneViewToolbar::UpdateSnapButtonState()
{
    if (auto* btn = dynamic_cast<Button*>(FindById("SnapToggle")))
    {
        if (m_Controller && m_Controller->IsGridSnapEnabled())
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}

void SceneViewToolbar::UpdateView2DButtonState()
{
    if (auto* btn = dynamic_cast<Button*>(FindById("View2DToggle")))
    {
        if (m_Controller && m_Controller->Is2DTarget())
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}

void SceneViewToolbar::UpdatePickModeButtonState()
{
    if (auto* btn = dynamic_cast<Button*>(FindById("PickModeToggle")))
    {
        if (Editor::SceneViewSettings::Get().GetExactPickMode())
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}

void SceneViewToolbar::UpdatePostProcessButtonState()
{
    if (auto* btn = dynamic_cast<Button*>(FindById("PostProcessToggle")))
    {
        if (m_Controller && m_Controller->IsPostProcessingEnabled())
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}

void SceneViewToolbar::UpdateProjectionButtonState()
{
    if (auto* btn = dynamic_cast<Button*>(FindById("ProjectionToggle")))
    {
        if (m_Controller && m_Controller->IsOrthographic())
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}

void SceneViewToolbar::UpdateRotationGizmoButtonState()
{
    if (auto* btn = dynamic_cast<Button*>(FindById("RotationGizmoToggle")))
    {
        if (Editor::SceneViewSettings::Get().GetShowRotationGizmo())
            btn->AddClass("icon-active");
        else
            btn->RemoveClass("icon-active");
    }
}

void SceneViewToolbar::RefreshButtonStates()
{
    UpdateGizmoButtonState();
    UpdateGridButtonState();
    UpdateSnapButtonState();
    UpdateView2DButtonState();
    UpdatePickModeButtonState();
    UpdatePostProcessButtonState();
    UpdateProjectionButtonState();
    UpdateRotationGizmoButtonState();
}

bool SceneViewToolbar::BeginWiringPass()
{
    m_WiringPassOpen = false;

    UIManager* owner = GetOwnerManager();
    const std::uint64_t generation = owner ? owner->GetTreeStructureGeneration() : 0;

    // Skip the pass outright when nothing in this subtree can need wiring.
    //
    // What makes that sound: AddChild / InsertChild / RemoveChild / TakeChild all call
    // UIManager::NotifyTreeStructureChanged, so every path that mounts, unmounts or
    // replaces a button inside this toolbar bumps the generation and the pass it
    // happens on always walks. The generation is manager-wide, so unrelated panels
    // force a walk too — conservative in the safe direction.
    //
    // The one structural path that does NOT bump is Mount::SwapTargetForActivation
    // (a dock tab activation must not trigger a dockspace rebuild). It is sound here
    // only because of WHERE it is called from: its two callers are both in
    // DockspaceElement, swapping a whole docked panel, so any such Mount is an
    // ANCESTOR of this toolbar and never inside it. Swapping the scene view panel out
    // and back re-parents the toolbar wholesale and preserves every button instance,
    // so nothing needs re-wiring.
    //
    // The contingency, stated because it would fail silently: a Mount placed INSIDE
    // the toolbar's own subtree would break this. Its swap would replace button
    // instances without bumping, and this skip would strand them with no crash and no
    // failing test. There is none today; adding one means revisiting this gate.
    //
    // m_WiringAllResolved is defensive rather than load-bearing — it keeps the toolbar
    // walking while any id is still missing, which costs walks during startup and buys
    // margin against a structural path that does not bump. Dropping it leaves the
    // wiring suite green.
    //
    // With no owning manager there is no generation to trust, so never skip.
    if (owner && m_WiringAllResolved && generation == m_WiringStructureGeneration)
        return false;

    ResolveWiredButtons();
    m_WiringStructureGeneration = generation;
    m_WiringPassOpen = true;
    return true;
}

void SceneViewToolbar::ResolveWiredButtons()
{
    for (WiredButton& wired : m_WiredButtons)
        wired.Resolved = nullptr;

    std::size_t unresolved = m_WiredButtons.size();

    const auto visit = [&](auto& self, UIElement* el) -> void {
        if (!el || unresolved == 0)
            return;

        if (const StringId elementId = el->GetIdHash(); elementId != StringId{0})
        {
            for (WiredButton& wired : m_WiredButtons)
            {
                if (wired.Id != elementId || wired.Resolved)
                    continue;
                // A non-Button carrying the id leaves the entry unresolved, so the walk
                // goes on and a later Button with the same id claims it. FindById-then-
                // cast would instead stop at that first element and yield nullptr
                // forever. The two diverge only for a DUPLICATED id, which is
                // ill-defined either way; preferring the Button is the more useful of
                // the two answers, and the toolbar has no duplicate ids.
                if (auto* btn = dynamic_cast<Button*>(el))
                {
                    wired.Resolved = btn;
                    --unresolved;
                }
                break;
            }
        }

        for (const auto& child : el->GetChildren())
            self(self, child.get());
        if (el->Kind() == UIElementKind::Mount)
            self(self, static_cast<Mount*>(el)->GetTarget());
    };
    visit(visit, this);

    m_WiringAllResolved = !m_WiredButtons.empty() && unresolved == 0;
}

Button* SceneViewToolbar::AcquireUnwiredButton(std::string_view id)
{
    if (!m_WiringPassOpen)
        return nullptr;

    const StringId key = HashStringId(id);
    WiredButton* wired = nullptr;
    for (WiredButton& candidate : m_WiredButtons)
    {
        if (candidate.Id == key)
        {
            wired = &candidate;
            break;
        }
    }

    if (!wired)
    {
        // First time any pass has asked for this id. Register it and resolve it on its
        // own; the batch walk covers it from the next pass on. Learning the wanted set
        // from the call sites is what keeps it from drifting out of step with them.
        m_WiredButtons.push_back(WiredButton{key, 0, dynamic_cast<Button*>(FindById(id))});
        wired = &m_WiredButtons.back();
        if (!wired->Resolved)
            m_WiringAllResolved = false;
    }

    Button* btn = wired->Resolved;
    if (!btn || wired->WiredInstanceId == btn->GetInstanceId())
        return nullptr;
    wired->WiredInstanceId = btn->GetInstanceId();
    return btn;
}

void SceneViewToolbar::WireControls()
{
    // Runs on every layout; AcquireUnwiredButton is what makes that safe by keeping each
    // binding one-shot per button instance. A re-entrant pass — a click handler flips a
    // CSS class, which relayouts and dispatches OnPostLayout — must not replace the
    // lambda still on the stack, and Update*ButtonState must not churn classes mid-layout.
    //
    // Resolving every id up front is safe against a handler run earlier in this pass
    // destroying a button resolved for a later one: OnPostLayout dispatches inside
    // UIElement::SetInEventDispatch(true), under which RemoveChild defers and TakeChild
    // refuses, so no element in this subtree can die before the pass closes.
    //
    // The re-entrancy this guards is a later relayout dispatching OnPostLayout again,
    // never a nested call inside one pass — which is why m_WiringPassOpen is a bool and
    // not a depth. A pass nested inside a pass would close the outer one's flag early;
    // nothing can produce one today, since the calls below only assign handlers and
    // set tooltips, and a relayout they trigger is deferred rather than synchronous.
    if (!BeginWiringPass())
        return;

    if (auto* spacerBtn = AcquireUnwiredButton("ToolbarSpacer1"))
        spacerBtn->SetTooltip("Toolbar spacer — drag to reposition this gap");

    // Wire gizmo toggle button
    if (auto* gizmoBtn = AcquireUnwiredButton("GizmoToggle"))
    {
        gizmoBtn->SetTooltip("Toggle Gizmos (right-click for options)");
        UpdateGizmoButtonState();

        gizmoBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            UIElement& btn = *e.CurrentTarget;
            if (m_Controller)
            {
                m_Controller->ToggleGizmos();
                if (m_Controller->AreGizmosVisible())
                    btn.AddClass("icon-active");
                else
                    btn.RemoveClass("icon-active");
            }
        });
        // Per-type gizmo visibility. Computed per show: without a controller there is no
        // state to toggle, so the menu suppresses itself.
        gizmoBtn->AddManipulator(ContextMenuManipulator::Create(
            [this]() -> std::vector<ContextMenuManipulator::Item> {
                SceneViewController* controller = m_Controller;
                if (!controller)
                    return {};
                return {
                    {.Path = "Transform Gizmos",
                     .IconPath = EditorIcons::kMove,
                     .Flags = CheckedFlag(controller->AreTransformGizmosVisible()),
                     .OnActivate =
                         [controller] {
                             controller->SetTransformGizmosVisible(
                                 !controller->AreTransformGizmosVisible());
                         }},
                    {.Path = "Light Gizmos",
                     .IconPath = EditorIcons::kLight,
                     .Flags = CheckedFlag(controller->AreLightGizmosVisible()),
                     .OnActivate =
                         [controller] {
                             controller->SetLightGizmosVisible(
                                 !controller->AreLightGizmosVisible());
                         }},
                    {.Path = "Frame Guide",
                     .IconPath = EditorIcons::kVideoCam,
                     .Flags = CheckedFlag(controller->GetCameraFrameGuide()),
                     .OnActivate =
                         [controller]
                         { controller->SetCameraFrameGuide(!controller->GetCameraFrameGuide()); }},
                    {.Path = "Selection Outlines",
                     .IconPath = EditorIcons::kPointer,
                     .Flags = CheckedFlag(
                         Editor::SceneViewSettings::Get().GetSelectionOutlinesVisible()),
                     .OnActivate =
                         [] {
                             auto& settings = Editor::SceneViewSettings::Get();
                             settings.SetSelectionOutlinesVisible(
                                 !settings.GetSelectionOutlinesVisible());
                         }},
                };
            }));
    }

    // Wire grid toggle button
    if (auto* gridBtn = AcquireUnwiredButton("GridToggle"))
    {
        gridBtn->SetTooltip("Toggle Grid (right-click for options)");
        UpdateGridButtonState();
        gridBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            UIElement& btn = *e.CurrentTarget;
            if (m_Controller)
            {
                m_Controller->ToggleGrid();
                if (m_Controller->IsGridVisible())
                    btn.AddClass("icon-active");
                else
                    btn.RemoveClass("icon-active");
            }
        });
        gridBtn->AddManipulator(ContextMenuManipulator::Create(
            [this]() -> std::vector<ContextMenuManipulator::Item> {
                SceneViewController* controller = m_Controller;
                if (!controller)
                    return {};
                return {
                    {.Path = "Show Grid",
                     .IconPath = EditorIcons::kEye,
                     .Flags = CheckedFlag(controller->IsGridVisible()),
                     .OnActivate =
                         [this, controller] {
                             controller->ToggleGrid();
                             UpdateGridButtonState();
                         }},
                    {.Path = "Snap to Grid",
                     .IconPath = EditorIcons::kNavGrid,
                     .Flags = CheckedFlag(controller->IsGridSnapEnabled()),
                     .OnActivate =
                         [this, controller] {
                             controller->ToggleGridSnap();
                             UpdateSnapButtonState();
                         }},
                    {.Separator = true},
                    {.Path = "Open Grid Settings...",
                     .IconPath = EditorIcons::kFolderOpen,
                     .OnActivate =
                         [this] {
                             if (m_OnOpenGridSettings)
                                 m_OnOpenGridSettings();
                         }},
                };
            }));
    }

    // Wire grid snap toggle button
    if (auto* snapBtn = AcquireUnwiredButton("SnapToggle"))
    {
        snapBtn->SetTooltip("Toggle Grid Snapping (right-click for options)");
        UpdateSnapButtonState();
        snapBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            UIElement& btn = *e.CurrentTarget;
            if (m_Controller)
            {
                m_Controller->ToggleGridSnap();
                if (m_Controller->IsGridSnapEnabled())
                    btn.AddClass("icon-active");
                else
                    btn.RemoveClass("icon-active");
            }
        });
        snapBtn->AddManipulator(ContextMenuManipulator::Create(
            [this]() -> std::vector<ContextMenuManipulator::Item> {
                SceneViewController* controller = m_Controller;
                if (!controller)
                    return {};
                std::vector<ContextMenuManipulator::Item> items;
                items.push_back({.Path = "Snap to Grid",
                                 .IconPath = EditorIcons::kNavGrid,
                                 .Flags = CheckedFlag(controller->IsGridSnapEnabled()),
                                 .OnActivate =
                                     [this, controller] {
                                         controller->ToggleGridSnap();
                                         UpdateSnapButtonState();
                                     }});
                items.push_back({.Separator = true});
                const float currentSize = controller->GetGridSnapSize();
                for (const float size : kSceneSnapSizePresets)
                {
                    std::ostringstream label;
                    label << "Snap Size: " << size;
                    items.push_back(
                        {.Path = label.str(),
                         .Flags = CheckedFlag(std::fabs(size - currentSize) < 1e-4f),
                         .OnActivate =
                             [this, controller, size] {
                                 controller->SetGridSnapSize(size);
                                 // Selecting a size turns snapping on so the choice is
                                 // immediately visible.
                                 if (!controller->IsGridSnapEnabled())
                                 {
                                     controller->ToggleGridSnap();
                                     UpdateSnapButtonState();
                                 }
                             }});
                }
                items.push_back({.Separator = true});
                items.push_back({.Path = "Open Snap Settings...",
                                 .IconPath = EditorIcons::kFolderOpen,
                                 .OnActivate =
                                     [this] {
                                         if (m_OnOpenGridSettings)
                                             m_OnOpenGridSettings();
                                     }});
                return items;
            }));
    }

    // Wire 2D/3D toggle button. Left-click toggles 2D view; right-click opens
    // a context menu with Pixel-Perfect options (Off / 1x-6x).
    if (auto* btn = AcquireUnwiredButton("View2DToggle"))
    {
        btn->SetTooltip("Toggle 2D / 3D View (right-click for Pixel-Perfect)");
        UpdateView2DButtonState();
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            UIElement& b = *e.CurrentTarget;
            bool is2D = false;
            if (m_OnToggleView2D)
            {
                is2D = m_OnToggleView2D();
            }
            else if (m_Controller)
            {
                m_Controller->Toggle2DMode();
                is2D = m_Controller->Is2DTarget();
            }
            if (is2D)
                b.AddClass("icon-active");
            else
                b.RemoveClass("icon-active");
        });
        btn->AddManipulator(ContextMenuManipulator::Create(
            [this]() -> std::vector<ContextMenuManipulator::Item> {
                SceneViewController* controller = m_Controller;
                // Pixel-perfect scaling is a 2D-only feature; the menu has nothing to
                // offer while the scene view is in 3D mode, so it suppresses itself.
                if (!controller || !controller->Is2DMode())
                    return {};

                const auto& settings = Editor::SceneViewSettings::Get();
                const bool ppOn = settings.GetPixelPerfect2D();
                const int ppScale = settings.GetPixelPerfectScale();
                // Pixel-perfect changes reach the render target through the controller
                // driving the ACTIVE viewport, read at invoke time.
                const auto refreshPixelPerfect = [this] {
                    if (m_Controller)
                        m_Controller->RefreshPixelPerfect2D();
                };

                std::vector<ContextMenuManipulator::Item> items;
                items.push_back({.Path = "Show Rulers",
                                 .IconPath = EditorIcons::kEye,
                                 .Flags = CheckedFlag(settings.GetShowRulers()),
                                 .OnActivate =
                                     [] {
                                         auto& s = Editor::SceneViewSettings::Get();
                                         s.SetShowRulers(!s.GetShowRulers());
                                     }});
                items.push_back({.Separator = true});
                items.push_back({.Path = "Pixel Perfect: Off",
                                 .Flags = CheckedFlag(!ppOn),
                                 .OnActivate =
                                     [refreshPixelPerfect] {
                                         Editor::SceneViewSettings::Get().SetPixelPerfect2D(false);
                                         refreshPixelPerfect();
                                     }});
                items.push_back({.Separator = true});
                for (int i = 0; i <= 6; ++i)
                {
                    const int scale = 1 << i; // 1, 2, 4, 8, 16, 32, 64
                    items.push_back({.Path = std::to_string(scale) + "x",
                                     .Flags = CheckedFlag(ppOn && ppScale == scale),
                                     .OnActivate =
                                         [refreshPixelPerfect, scale] {
                                             auto& s = Editor::SceneViewSettings::Get();
                                             s.SetPixelPerfect2D(true);
                                             s.SetPixelPerfectScale(scale);
                                             refreshPixelPerfect();
                                         }});
                }
                return items;
            }));
    }

    // Wire pick mode toggle: active = exact pick (select the entity under the
    // cursor), inactive = root pick (select the model-instance root).
    if (auto* btn = AcquireUnwiredButton("PickModeToggle"))
    {
        btn->SetTooltip("Pick Mode (T) — active: select exact entity; inactive: select model root");
        UpdatePickModeButtonState();
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            auto& settings = Editor::SceneViewSettings::Get();
            settings.SetExactPickMode(!settings.GetExactPickMode());
            UpdatePickModeButtonState();
        });
    }

    // Wire perspective/orthographic projection toggle
    if (auto* btn = AcquireUnwiredButton("ProjectionToggle"))
    {
        btn->SetTooltip("Toggle Perspective / Orthographic");
        UpdateProjectionButtonState();
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            UIElement& b = *e.CurrentTarget;
            if (!m_Controller)
                return;
            m_Controller->ToggleOrthographic();
            if (m_Controller->IsOrthographic())
                b.AddClass("icon-active");
            else
                b.RemoveClass("icon-active");
        });
    }

    // Wire rotation gizmo toggle button (toggles the top-right viewport axis widget)
    if (auto* btn = AcquireUnwiredButton("RotationGizmoToggle"))
    {
        btn->SetTooltip("Toggle Viewport Rotation Gizmo (right-click for placement)");
        UpdateRotationGizmoButtonState();
        btn->RegisterEventHandler(kEventButtonClick, [](UIEvent& e) {
            UIElement& b = *e.CurrentTarget;
            const bool next = !Editor::SceneViewSettings::Get().GetShowRotationGizmo();
            Editor::SceneViewSettings::Get().SetShowRotationGizmo(next);
            if (next)
                b.AddClass("icon-active");
            else
                b.RemoveClass("icon-active");
        });
        // Fixed item set, live state: corner placement plus per-split-view visibility, all
        // in SceneViewSettings, so the hooks need no controller.
        std::vector<ContextMenuManipulator::Item> items;
        using Corner = Editor::RotationGizmoCorner;
        static constexpr struct
        {
            const char* Label;
            Corner Value;
        } kCorners[] = {
            {"Top Right", Corner::TopRight},
            {"Top Left", Corner::TopLeft},
            {"Bottom Right", Corner::BottomRight},
            {"Bottom Left", Corner::BottomLeft},
        };
        for (const auto& corner : kCorners)
        {
            items.push_back(
                {.Path = corner.Label,
                 .IconPath = EditorIcons::kNavGizmo,
                 .OnActivate = [value = corner.Value]
                 { Editor::SceneViewSettings::Get().SetRotationGizmoCorner(value); },
                 .State = [value = corner.Value] {
                     return ContextMenuManipulator::ItemState{
                         .Checked = Editor::SceneViewSettings::Get().GetRotationGizmoCorner() ==
                                    value};
                 }});
        }
        items.push_back({.Separator = true});
        items.push_back({.Path = "Show In Split View", .IconPath = EditorIcons::kEye});
        for (size_t i = 0; i < std::size(kRotationGizmoSplitLabels); ++i)
        {
            items.push_back(
                {.Path = std::string("Show In Split View/") + kRotationGizmoSplitLabels[i],
                 .IconPath = EditorIcons::kEye,
                 .OnActivate =
                     [i] {
                         auto& settings = Editor::SceneViewSettings::Get();
                         settings.SetShowRotationGizmoForViewport(
                             i, !settings.GetShowRotationGizmoForViewport(i));
                     },
                 .State = [i] {
                     return ContextMenuManipulator::ItemState{
                         .Checked =
                             Editor::SceneViewSettings::Get().GetShowRotationGizmoForViewport(i)};
                 }});
        }
        btn->AddManipulator(ContextMenuManipulator::Create(std::move(items)));
    }

    // Wire tool overlay toggle button
    if (auto* btn = AcquireUnwiredButton("ToolOverlayToggle"))
    {
        btn->SetTooltip("Toggle Tool Overlay");
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            UIElement& b = *e.CurrentTarget;
            // Find the SceneToolOverlay by walking up to the panel root
            UIElement* panel = GetParent();
            while (panel && panel->GetParent())
                panel = panel->GetParent();
            if (!panel)
                return;
            auto* overlay = panel->FindById("SceneToolOverlay");
            auto* inlineButtons = FindById("InlineToolButtons");
            bool isActive = b.HasClass("icon-active");
            if (isActive)
            {
                if (overlay)
                    overlay->AddClass("hidden");
                if (inlineButtons)
                    inlineButtons->RemoveClass("hidden");
                b.RemoveClass("icon-active");
            }
            else
            {
                if (overlay)
                    overlay->RemoveClass("hidden");
                if (inlineButtons)
                    inlineButtons->AddClass("hidden");
                b.AddClass("icon-active");
            }
        });
    }

    // Wire the post-processing master toggle
    if (auto* btn = AcquireUnwiredButton("PostProcessToggle"))
    {
        btn->SetTooltip("Toggle all Scene View post processing (right-click for per-effect options)");
        UpdatePostProcessButtonState();
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            if (!m_Controller)
                return;
            m_Controller->TogglePostProcessing();
            UpdatePostProcessButtonState();
        });
        btn->AddManipulator(ContextMenuManipulator::Create(
            [this]() -> std::vector<ContextMenuManipulator::Item> {
                return Editor::BuildScenePostFxMenuItems(m_Controller, this,
                                                         m_OnPostFxAutoExposureToggled);
            }));
    }

    // Wire the scene camera quick-settings button
    if (auto* btn = AcquireUnwiredButton("CameraSettingsBtn"))
    {
        btn->SetTooltip("Scene View camera settings (FOV, clipping, speed, exposure)");
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
            UIElement& b = *e.CurrentTarget;
            if (m_OnOpenCameraSettings)
            {
                // Button center/bottom so the popup's anchor arrow points at it.
                const float x = b.GetLayoutX() + b.GetLayoutWidth() * 0.5f;
                const float y = b.GetLayoutY() + b.GetLayoutHeight();
                auto cb = m_OnOpenCameraSettings;
                b.PostAction([cb, x, y]() { cb(x, y); });
            }
        });
    }

    // Close the pass. Resolved pointers are pass-scoped by contract, so dropping them
    // here means the toolbar never holds an element pointer between passes and a
    // skipped pass has nothing stale to hand back.
    for (WiredButton& wired : m_WiredButtons)
        wired.Resolved = nullptr;
    m_WiringPassOpen = false;
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_sceneViewToolbar =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewToolbar>(
        "SceneViewToolbar",
        []() { return std::make_unique<GameEngine::SceneViewToolbar>(); })
        .TagAlias("sceneviewtoolbar");
}
