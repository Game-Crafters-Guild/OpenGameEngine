#include "UI/Controls/LayoutPresetToolbar.h"

#include "UI/Controls/Button.h"
#include "UI/EditorIcons.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Registration/ElementRegistration.h"
#include "Input/KeyCodes.h"

#include <cstring>

namespace GameEngine
{

namespace
{
static void AddIconClassForPresetIndex(Button& btn, int index)
{
    // Keep icon selection stable and cycle for indices beyond the first 4.
    const int iconIndex = (index <= 3) ? index : ((index - 4) % 4);
    switch (iconIndex)
    {
        case 0: btn.AddClass("layout-left-icon"); break;
        case 1: btn.AddClass("layout-right-icon"); break;
        case 2: btn.AddClass("layout-3split-icon"); break;
        case 3: btn.AddClass("layout-top-icon"); break;
        default: break;
    }
}
} // namespace

LayoutPresetToolbar::LayoutPresetToolbar()
{
    // Ensure expected styling even if layout forgets to set the class.
    AddClass("bottom-toolbar-center");
}

void LayoutPresetToolbar::SetPresets(std::vector<std::string> presetNames)
{
    m_PresetNames = std::move(presetNames);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetActiveIndex(int index)
{
    m_ActiveIndex = index;
    UpdateActiveClasses();
}

void LayoutPresetToolbar::SetOnRecallPreset(std::function<void(int)> callback)
{
    m_OnRecallPreset = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetOnSavePreset(std::function<void()> callback)
{
    m_OnSavePreset = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetOnRemovePreset(std::function<void(int)> callback)
{
    m_OnRemovePreset = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetOnOverwritePreset(std::function<void(int)> callback)
{
    m_OnOverwritePreset = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetOnRenamePreset(std::function<void(int)> callback)
{
    m_OnRenamePreset = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetOnExportPreset(std::function<void(int)> callback)
{
    m_OnExportPreset = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetOnExportAllPresets(std::function<void()> callback)
{
    m_OnExportAllPresets = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::SetOnImportPresets(std::function<void()> callback)
{
    m_OnImportPresets = std::move(callback);
    m_NeedsRebuild = true;
    ScheduleRebuild();
}

void LayoutPresetToolbar::OnPostLayout()
{
    if (m_NeedsRebuild)
    {
        ScheduleRebuild();
    }
}

void LayoutPresetToolbar::ScheduleRebuild()
{
    if (m_RebuildScheduled)
        return;

    m_RebuildScheduled = true;
    PostAction([this]()
               {
                   m_RebuildScheduled = false;
                   RebuildNow();
               });
}

void LayoutPresetToolbar::RebuildNow()
{
    m_NeedsRebuild = false;

    // Clear existing children.
    RemoveAllChildren();

    // Recreate preset buttons.
    for (int i = 0; i < static_cast<int>(m_PresetNames.size()); ++i)
    {
        auto btn = std::make_unique<Button>();
        btn->SetId("LayoutPreset_" + std::to_string(i));
        btn->AddClass("small");
        btn->AddClass("secondary");
        btn->AddClass("layout-button");
        AddIconClassForPresetIndex(*btn, i);
        btn->SetText(m_PresetNames[(size_t)i]);

        if (i > 0)
            btn->SetTooltip("Click to recall. Ctrl/Cmd+click to overwrite. Right-click for options.");
        else
            btn->SetTooltip("Recall default layout. Right-click for options.");

        if (i == m_ActiveIndex)
        {
            btn->AddClass("active");
        }

        btn->RegisterEventHandler(kEventButtonClick, [this, i](UIEvent& e)
                        {
                            // Ctrl/Cmd+click on non-default presets overwrites with current layout.
                            const int mods = e.Mods;
                            if (i > 0 && Input::IsPrimaryShortcutModifier(mods))
                            {
                                this->PostAction([this, i]()
                                                 {
                                                     if (m_OnOverwritePreset)
                                                         m_OnOverwritePreset(i);
                                                 });
                                return;
                            }
                            // Normal click recalls the preset.
                            this->PostAction([this, i]()
                                             {
                                                 if (m_OnRecallPreset)
                                                     m_OnRecallPreset(i);
                                             });
                        });

        // Rename/Delete apply to the clicked preset; the default layout (index 0) offers
        // neither. Export All needs a second preset to be worth a file.
        const auto isUserPreset = [this, i]
        { return i > 0 && i < static_cast<int>(m_PresetNames.size()); };
        btn->AddManipulator(ContextMenuManipulator::Create({
            {.Path = "Rename...",
             .IconPath = EditorIcons::kBrush,
             .OnActivate =
                 [this, i]
                 {
                     if (m_OnRenamePreset)
                         m_OnRenamePreset(i);
                 },
             .State =
                 [isUserPreset] {
                     return ContextMenuManipulator::ItemState{.Enabled = isUserPreset()};
                 }},
            {.Path = "Delete",
             .IconPath = EditorIcons::kTrash,
             .OnActivate =
                 [this, i]
                 {
                     if (m_OnRemovePreset)
                         m_OnRemovePreset(i);
                 },
             .State =
                 [isUserPreset] {
                     return ContextMenuManipulator::ItemState{.Enabled = isUserPreset()};
                 }},
            {.Separator = true},
            {.Path = "Export Current Layout...",
             .IconPath = EditorIcons::kSave,
             .OnActivate =
                 [this, i]
                 {
                     if (m_OnExportPreset)
                         m_OnExportPreset(i);
                 }},
            {.Path = "Export All Layouts...",
             .IconPath = EditorIcons::kSave,
             .OnActivate =
                 [this]
                 {
                     if (m_OnExportAllPresets)
                         m_OnExportAllPresets();
                 },
             .State =
                 [this] {
                     return ContextMenuManipulator::ItemState{.Enabled =
                                                                  m_PresetNames.size() > 1};
                 }},
            {.Path = "Import Layouts...",
             .IconPath = EditorIcons::kFolderOpen,
             .OnActivate =
                 [this]
                 {
                     if (m_OnImportPresets)
                         m_OnImportPresets();
                 }},
        }));

        AddChild(std::move(btn));
    }

    // Plus ("save preset") button.
    {
        auto plus = std::make_unique<Button>();
        plus->SetId("AddLayoutButton");
        plus->AddClass("small");
        plus->AddClass("secondary");
        plus->AddClass("icon-button");
        plus->AddClass("plus-icon");
        plus->SetTooltip("Save Layout Preset");
        plus->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                         {
                             this->PostAction([this]()
                                              {
                                                  if (m_OnSavePreset)
                                                      m_OnSavePreset();
                                              });
                         });
        AddChild(std::move(plus));
    }

    MarkDirty(ChildrenDirty | LayoutDirty | VisualDirty);
}

void LayoutPresetToolbar::UpdateActiveClasses()
{
    // Update active class on preset buttons.
    constexpr const char* kPrefix = "LayoutPreset_";
    for (const auto& child : GetChildren())
    {
        if (!child)
            continue;

        const std::string& id = child->GetId();
        if (id.rfind(kPrefix, 0) != 0)
            continue;

        int index = -1;
        try
        {
            index = std::stoi(id.substr(std::strlen(kPrefix)));
        }
        catch (...)
        {
            index = -1;
        }

        if (index == m_ActiveIndex)
        {
            if (!child->HasClass("active"))
            {
                child->AddClass("active");
                child->MarkDirty(UIElement::VisualDirty);
            }
        }
        else
        {
            if (child->HasClass("active"))
            {
                child->RemoveClass("active");
                child->MarkDirty(UIElement::VisualDirty);
            }
        }
    }
}

} // namespace GameEngine

namespace
{
using namespace GameEngine;
using namespace GameEngine::UIRegistration;

static auto s_reg_layoutPresetToolbar =
    RegisterWithFactory<LayoutPresetToolbar>(
        "LayoutPresetToolbar",
        []() { return std::make_unique<LayoutPresetToolbar>(); })
        .TagAlias("layoutpresettoolbar");
} // namespace

