// SceneViewPanel's tool-button menus — the select and measure item sets, declared for the
// manipulators attached in SceneViewPanel.cpp (the quad-view set lives with the rest of
// the quad code in SceneViewPanelQuad.cpp). Both are fixed sets whose state hooks re-read
// the panel and its settings at every show.

#include "Panels/SceneViewPanel.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "UI/ContextMenuLabels.h"
#include "UI/EditorIcons.h"

#include <string>
#include <vector>

namespace GameEngine
{

namespace
{

using Item = ContextMenuManipulator::Item;
using ItemState = ContextMenuManipulator::ItemState;
using namespace Editor::ContextMenuLabels;

constexpr float kSceneSelectThicknessPresets[] = {1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f};

} // namespace

std::vector<ContextMenuManipulator::Item> SceneViewPanel::BuildSelectToolMenuItems()
{
    std::vector<Item> items;

    // ---- Marquee (drag-select rectangle/lasso) appearance ----
    items.push_back({.Path = "Drag Selection Box",
                     .IconPath = EditorIcons::kPointer,
                     .OnActivate =
                         [] {
                             Editor::SceneViewSettings::Get().SetMarqueeShape(
                                 Editor::MarqueeShape::Rectangle);
                         },
                     .State = [] {
                         return ItemState{.Checked =
                                              Editor::SceneViewSettings::Get().GetMarqueeShape() ==
                                              Editor::MarqueeShape::Rectangle};
                     }});
    items.push_back({.Path = "Free-form (Lasso)",
                     .IconPath = EditorIcons::kPointer,
                     .OnActivate =
                         [] {
                             Editor::SceneViewSettings::Get().SetMarqueeShape(
                                 Editor::MarqueeShape::Lasso);
                         },
                     .State = [] {
                         return ItemState{.Checked =
                                              Editor::SceneViewSettings::Get().GetMarqueeShape() ==
                                              Editor::MarqueeShape::Lasso};
                     }});

    items.push_back({.Path = "Marquee Thickness", .IconPath = EditorIcons::kSplineCurve});
    for (const float v : kSceneSelectThicknessPresets)
    {
        items.push_back({.Path = "Marquee Thickness/" + FloatLabel(v),
                         .OnActivate =
                             [v] { Editor::SceneViewSettings::Get().SetMarqueeThickness(v); },
                         .State = [v] {
                             return ItemState{.Checked = NearlyEqual(
                                                  v, Editor::SceneViewSettings::Get()
                                                         .GetMarqueeThickness())};
                         }});
    }
    items.push_back(
        {.Path = "Marquee Color...",
         .IconPath = EditorIcons::kColorPicker,
         .OnActivate =
             [this]
             {
                 if (!m_OpenColorPickerWindow)
                     return;
                 ColorPickerCallbacks cbs;
                 cbs.onApply = [](uint32_t argb, float /*intensity*/)
                 { Editor::SceneViewSettings::Get().SetMarqueeColor(argb); };
                 cbs.onValueChanging = [](uint32_t argb, float /*intensity*/)
                 { Editor::SceneViewSettings::Get().SetMarqueeColor(argb); };
                 m_OpenColorPickerWindow(Editor::SceneViewSettings::Get().GetMarqueeColor(), 1.0f,
                                         std::move(cbs));
             },
         .State = [] {
             return ItemState{.ColorHex = ArgbToHexRGB(
                                  Editor::SceneViewSettings::Get().GetMarqueeColor())};
         }});

    items.push_back({.Separator = true});

    // ---- Selection Highlight (parent submenu) ----
    // Style chooser at the top, then nested Bounding Box and Outline submenus each holding
    // their own color/thickness controls. The style gates which halves are enabled.
    items.push_back({.Path = "Selection Highlight", .IconPath = EditorIcons::kPointer});

    const struct
    {
        const char* Label;
        const char* Icon;
        Editor::SelectionHighlightStyle Style;
    } kStyles[] = {
        {"Selection Highlight/Bounding Box", EditorIcons::kBox,
         Editor::SelectionHighlightStyle::Box},
        {"Selection Highlight/Silhouette Outline", EditorIcons::kPointer,
         Editor::SelectionHighlightStyle::Outline},
        {"Selection Highlight/Both", EditorIcons::kPointer, Editor::SelectionHighlightStyle::Both},
    };
    for (const auto& style : kStyles)
    {
        items.push_back({.Path = style.Label,
                         .IconPath = style.Icon,
                         .OnActivate = [value = style.Style]
                         { Editor::SceneViewSettings::Get().SetSelectionHighlightStyle(value); },
                         .State = [value = style.Style] {
                             return ItemState{
                                 .Checked = Editor::SceneViewSettings::Get()
                                                .GetSelectionHighlightStyle() == value};
                         }});
    }

    items.push_back({.Path = "Selection Highlight", .Separator = true});

    const auto boxEnabled = []
    {
        return Editor::SceneViewSettings::Get().GetSelectionHighlightStyle() !=
               Editor::SelectionHighlightStyle::Outline;
    };
    const auto outlineEnabled = []
    {
        return Editor::SceneViewSettings::Get().GetSelectionHighlightStyle() !=
               Editor::SelectionHighlightStyle::Box;
    };

    // Bounding-box subsubmenu
    items.push_back(
        {.Path = "Selection Highlight/Bounding Box Settings", .IconPath = EditorIcons::kSettings});
    items.push_back(
        {.Path = "Selection Highlight/Bounding Box Settings/Color...",
         .IconPath = EditorIcons::kColorPicker,
         .OnActivate =
             [this]
             {
                 if (!m_OpenColorPickerWindow)
                     return;
                 ColorPickerCallbacks cbs;
                 cbs.onApply = [](uint32_t argb, float /*intensity*/)
                 { Editor::SceneViewSettings::Get().SetSelectionBoxColor(argb); };
                 cbs.onValueChanging = [](uint32_t argb, float /*intensity*/)
                 { Editor::SceneViewSettings::Get().SetSelectionBoxColor(argb); };
                 m_OpenColorPickerWindow(Editor::SceneViewSettings::Get().GetSelectionBoxColor(),
                                         1.0f, std::move(cbs));
             },
         .State = [boxEnabled] {
             return ItemState{.Enabled = boxEnabled(),
                              .ColorHex = ArgbToHexRGB(
                                  Editor::SceneViewSettings::Get().GetSelectionBoxColor())};
         }});
    items.push_back({.Path = "Selection Highlight/Bounding Box Settings/Thickness",
                     .IconPath = EditorIcons::kSplineCurve});
    for (const float v : kSceneSelectThicknessPresets)
    {
        items.push_back(
            {.Path = "Selection Highlight/Bounding Box Settings/Thickness/" + FloatLabel(v),
             .OnActivate = [v]
             { Editor::SceneViewSettings::Get().SetSelectionBoxThickness(v); },
             .State = [v, boxEnabled] {
                 return ItemState{
                     .Checked = NearlyEqual(
                         v, Editor::SceneViewSettings::Get().GetSelectionBoxThickness()),
                     .Enabled = boxEnabled()};
             }});
    }

    // Outline subsubmenu
    items.push_back(
        {.Path = "Selection Highlight/Outline Settings", .IconPath = EditorIcons::kSettings});
    items.push_back(
        {.Path = "Selection Highlight/Outline Settings/Color...",
         .IconPath = EditorIcons::kColorPicker,
         .OnActivate =
             [this]
             {
                 if (!m_OpenColorPickerWindow)
                     return;
                 ColorPickerCallbacks cbs;
                 cbs.onApply = [](uint32_t argb, float /*intensity*/)
                 { Editor::SceneViewSettings::Get().SetSelectionOutlineColor(argb); };
                 cbs.onValueChanging = [](uint32_t argb, float /*intensity*/)
                 { Editor::SceneViewSettings::Get().SetSelectionOutlineColor(argb); };
                 m_OpenColorPickerWindow(
                     Editor::SceneViewSettings::Get().GetSelectionOutlineColor(), 1.0f,
                     std::move(cbs));
             },
         .State = [outlineEnabled] {
             return ItemState{.Enabled = outlineEnabled(),
                              .ColorHex = ArgbToHexRGB(
                                  Editor::SceneViewSettings::Get().GetSelectionOutlineColor())};
         }});
    items.push_back({.Path = "Selection Highlight/Outline Settings/Thickness",
                     .IconPath = EditorIcons::kSplineCurve});
    for (const float v : kSceneSelectThicknessPresets)
    {
        items.push_back(
            {.Path = "Selection Highlight/Outline Settings/Thickness/" + FloatLabel(v),
             .OnActivate = [v]
             { Editor::SceneViewSettings::Get().SetSelectionOutlineThickness(v); },
             .State = [v, outlineEnabled] {
                 return ItemState{
                     .Checked = NearlyEqual(
                         v, Editor::SceneViewSettings::Get().GetSelectionOutlineThickness()),
                     .Enabled = outlineEnabled()};
             }});
    }

    return items;
}

std::vector<ContextMenuManipulator::Item> SceneViewPanel::BuildMeasureMenuItems()
{
    std::vector<Item> items;

    items.push_back({.Path = "Create Measure Entity on Drag",
                     .IconPath = EditorIcons::kPlus,
                     .OnActivate =
                         [this]
                         {
                             m_MeasureCreateEntitiesMode = !m_MeasureCreateEntitiesMode;
                             Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
                             std::string err;
                             (void)prefs.Load(&err);
                             prefs.SetBool("sceneView.measure.createEntities",
                                           m_MeasureCreateEntitiesMode);
                             (void)prefs.Save(&err);
                         },
                     .State =
                         [this] {
                             return ItemState{.Checked = m_MeasureCreateEntitiesMode};
                         }});
    items.push_back({.Separator = true});
    items.push_back(
        {.Path = "Metric",
         .OnActivate = [this] { SetMeasureUnitSystem(SceneViewMeasureOverlay::UnitSystem::Metric); },
         .State =
             [this] {
                 return ItemState{.Checked = m_MeasureUnitSystem ==
                                             SceneViewMeasureOverlay::UnitSystem::Metric};
             }});
    items.push_back(
        {.Path = "Imperial",
         .OnActivate =
             [this] { SetMeasureUnitSystem(SceneViewMeasureOverlay::UnitSystem::Imperial); },
         .State =
             [this] {
                 return ItemState{.Checked = m_MeasureUnitSystem ==
                                             SceneViewMeasureOverlay::UnitSystem::Imperial};
             }});
    items.push_back({.Separator = true});
    items.push_back(
        {.Path = "2D Triangle",
         .OnActivate = [this] { SetMeasureTwoDMode(SceneViewMeasureOverlay::TwoDMode::Triangle); },
         .State =
             [this] {
                 return ItemState{.Checked = m_MeasureTwoDMode ==
                                             SceneViewMeasureOverlay::TwoDMode::Triangle};
             }});
    items.push_back(
        {.Path = "2D Two Points",
         .OnActivate = [this] { SetMeasureTwoDMode(SceneViewMeasureOverlay::TwoDMode::Points); },
         .State =
             [this] {
                 return ItemState{.Checked = m_MeasureTwoDMode ==
                                             SceneViewMeasureOverlay::TwoDMode::Points};
             }});
    items.push_back({.Separator = true});
    items.push_back({.Path = "Measure Color...",
                     .IconPath = EditorIcons::kColorPicker,
                     .OnActivate =
                         [this]
                         {
                             if (!m_OpenColorPickerWindow)
                                 return;
                             const uint32_t original =
                                 Editor::SceneViewSettings::Get().GetMeasureColor();
                             ColorPickerCallbacks cbs;
                             cbs.onApply = [](uint32_t argb, float /*intensity*/)
                             { Editor::SceneViewSettings::Get().SetMeasureColor(argb); };
                             cbs.onCancel = [original]()
                             { Editor::SceneViewSettings::Get().SetMeasureColor(original); };
                             cbs.onValueChanging = [](uint32_t argb, float /*intensity*/)
                             { Editor::SceneViewSettings::Get().SetMeasureColor(argb); };
                             m_OpenColorPickerWindow(original, 1.0f, std::move(cbs));
                         },
                     .State =
                         [this] {
                             return ItemState{.Enabled = m_OpenColorPickerWindow != nullptr};
                         }});

    return items;
}

} // namespace GameEngine
