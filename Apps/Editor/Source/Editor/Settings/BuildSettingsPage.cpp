#include "Editor/Settings/BuildSettingsPage.h"

#include "Editor/Assets/AssetRelativePath.h"
#include "Editor/Settings/BuildFolderDefaults.h"
#include "Editor/Settings/BuildRenderPipelineSettings.h"
#include "Editor/DragDropPayloads.h"
#include "Editor/Settings/BuildSettingsStore.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Panels/BookmarksPanel.h"
#include "Panels/BuildPanel.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Engine/Build/AppIconGenerator.h"
#include "Engine/Build/BuildPlatforms.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Build/PlayerBuildConfig.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/InfoCard.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

// Asset grid drag state, defined in BookmarksPanel.cpp — the drop slots below
// accept a plain asset-grid drag as well as a DragDropManager payload.
struct DragState
{
    bool active = false;
    std::filesystem::path assetPath;
    std::string scenePath;
    std::string entityId;
};
extern DragState g_DragState;

namespace Editor
{
namespace
{

constexpr const char* kBuildCategoryId = "build";

std::string PlatformCategoryId(const std::string& platformName)
{
    return std::string(kBuildCategoryId) + "." + platformName;
}

// ---------------------------------------------------------------------------
// Shared row scaffolding
// ---------------------------------------------------------------------------

std::unique_ptr<UIElement> MakeColumn(float marginTopPx, float gapPx)
{
    auto column = std::make_unique<UIElement>();
    column->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Gap, StyleLength::Px(gapPx))
        .Set(Style::MarginTop, StyleLength::Px(marginTopPx));
    return column;
}

std::unique_ptr<UIElement> MakeSettingsRow()
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("settings-row");
    row->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(8.0f));
    return row;
}

std::unique_ptr<Label> MakeRowLabel(const std::string& text, const char* tooltip = nullptr)
{
    auto label = std::make_unique<Label>();
    label->SetText(text);
    if (tooltip)
        label->SetTooltip(tooltip);
    label->AddClass("settings-row-label");
    return label;
}

// A toggle row that persists straight to the build settings store. It is handed
// the current value rather than a key plus a default, so the default keeps its
// single home in BuildSettingsStore.
void AppendStoreToggleRow(UIElement& parent, const std::string& labelText,
                          const std::string& prefKey, bool currentValue, const char* tooltip,
                          std::function<void(bool)> onChanged = {})
{
    auto row = MakeSettingsRow();
    row->AddChild(MakeRowLabel(labelText, tooltip));

    auto toggle = std::make_unique<Toggle>();
    toggle->SetChecked(currentValue);
    if (tooltip)
        toggle->SetTooltip(tooltip);
    toggle->SetOnValueChanged([prefKey, onChanged = std::move(onChanged)](const bool& checked)
    {
        SaveBuildBoolSetting(prefKey, checked);
        if (onChanged)
            onChanged(checked);
    });
    row->AddChild(std::move(toggle));
    parent.AddChild(std::move(row));
}

// Re-lays out a container after its visibility flipped. The flip is posted so
// it lands after the click that caused it has finished dispatching.
void SetBlockVisibleDeferred(UIElement* block, bool visible)
{
    if (!block)
        return;
    block->PostAction([block, visible]()
    {
        block->Overrides().Set(Style::Visibility, visible);
        block->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);
        if (UIElement* parent = block->GetParent())
            parent->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty |
                              UIElement::ChildrenDirty);
    });
}

// ---------------------------------------------------------------------------
// Render pipeline options
// ---------------------------------------------------------------------------

std::vector<SettingsFieldDescriptor::DropdownField::Option> CollectRenderPipelineOptions(
    const std::string& selectedPath)
{
    using Option = SettingsFieldDescriptor::DropdownField::Option;

    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    auto& registry = assetManager.GetRegistry();

    std::vector<Option> options;
    const auto guids = registry.GetAssetsByType(AssetType::RenderPipeline);
    options.reserve(guids.size() + 2);

    for (const GUID& guid : guids)
    {
        AssetMetadata metadata{};
        if (!registry.TryGetAssetMetadata(guid, metadata))
            continue;
        if (metadata.Type != AssetType::RenderPipeline)
            continue;

        const std::string rel = TryMakeAssetRelativePathString(assetManager, metadata.Path);
        if (rel.empty())
            continue;

        options.push_back(Option{rel, std::filesystem::path(rel).stem().string()});
    }

    std::sort(options.begin(), options.end(),
              [](const Option& a, const Option& b) { return a.Value < b.Value; });
    options.erase(std::unique(options.begin(), options.end(),
                              [](const Option& a, const Option& b) { return a.Value == b.Value; }),
                  options.end());
    std::sort(options.begin(), options.end(),
              [](const Option& a, const Option& b) { return a.Label < b.Label; });

    // A configured pipeline whose asset is gone stays selectable and is marked,
    // so opening the page cannot silently retarget the build.
    if (!selectedPath.empty())
    {
        const bool found = std::any_of(options.begin(), options.end(),
                                       [&](const Option& option) { return option.Value == selectedPath; });
        if (!found)
        {
            options.push_back(Option{selectedPath,
                                     std::filesystem::path(selectedPath).stem().string() +
                                         " (missing)"});
        }
    }

    if (options.empty())
        options.push_back(Option{kDefaultBuildRenderPipelinePath, "ForwardPlus"});

    return options;
}

// ---------------------------------------------------------------------------
// Scene list
// ---------------------------------------------------------------------------

// A bookmark as a single relative scene path, when it refers to a .scene asset.
std::vector<std::string> BookmarkToScenePaths(const Bookmark& bookmark)
{
    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    AssetRegistry& registry = assetManager.GetRegistry();
    std::vector<std::string> out;

    if (bookmark.Type == BookmarkType::Entity)
    {
        const size_t separator = bookmark.Reference.find('|');
        if (separator == std::string::npos)
            return out;
        const std::string scenePath = bookmark.Reference.substr(0, separator);
        if (scenePath.empty())
            return out;
        const std::filesystem::path path(scenePath);
        const std::string ext = path.extension().string();
        if (ext != ".scene" && ext != ".SCENE")
            return out;
        std::string rel = scenePath;
        if (path.is_absolute())
        {
            rel = TryMakeAssetRelativePathString(assetManager, path);
            if (rel.empty())
                return out;
        }
        out.push_back(std::move(rel));
        return out;
    }

    if (bookmark.Type == BookmarkType::Scene || bookmark.Type == BookmarkType::Asset)
    {
        if (bookmark.Reference.empty())
            return out;
        const GUID guid(bookmark.Reference);
        if (guid.IsNull())
            return out;
        AssetMetadata metadata;
        if (!registry.TryGetAssetMetadata(guid, metadata) || metadata.Path.empty())
            return out;
        const std::string ext = metadata.Path.extension().string();
        if (ext != ".scene" && ext != ".SCENE")
            return out;
        std::string rel = TryMakeAssetRelativePathString(assetManager, metadata.Path);
        if (!rel.empty())
            out.push_back(std::move(rel));
    }
    return out;
}

class BuildSceneListDropTarget final : public UIElement, public UI::Interaction::IDropTarget
{
  public:
    using OnDropFn = std::function<void(const std::vector<std::string>& paths)>;

    explicit BuildSceneListDropTarget(OnDropFn onDrop)
        : m_OnDrop(std::move(onDrop))
    {
        AddClass("settings-build-drop-zone");
        auto label = std::make_unique<Label>();
        label->SetText("Drop .scene assets or scene bookmarks here");
        label->AddClass("settings-description");
        EditorUI::StyleInfoCardText(label.get());
        label->Overrides()
            .Set(Style::PaddingTop, StyleLength::Px(4.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(4.0f));
        AddChild(std::move(label));
        Overrides().Set(Style::MinHeight, StyleLength::Px(28.0f));

        // Asset-grid drag (g_DragState), for the path that does not go through
        // DragDropManager.
        RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
        {
            if (e.Button != 0)
                return;
            if (!g_DragState.active || g_DragState.assetPath.empty())
                return;
            const std::string ext = g_DragState.assetPath.extension().string();
            if (ext != ".scene" && ext != ".SCENE")
                return;
            auto& assetManager = EngineCore::GetInstance().GetAssetManager();
            const std::string rel =
                TryMakeAssetRelativePathString(assetManager, g_DragState.assetPath);
            if (rel.empty())
                return;
            if (m_OnDrop)
                m_OnDrop({rel});
            g_DragState.active = false;
            g_DragState.assetPath.clear();
            e.Stop();
        });
    }

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override
    {
        return typeId == UI::Interaction::GetPayloadTypeId<AssetPathsDragPayload>() ||
               typeId == UI::Interaction::GetPayloadTypeId<BookmarkDragPayload>();
    }

    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override
    {
        if (!ContainsPoint(x, y))
            return false;
        out.TargetId = 0;
        out.Location = UI::Interaction::DropLocation::OnItem;
        return true;
    }

    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override
    {
        const auto* assetPayload = request.payload.TryGet<AssetPathsDragPayload>();
        if (assetPayload && !assetPayload->paths.empty())
        {
            for (const auto& path : assetPayload->paths)
            {
                std::error_code ec;
                if (path.empty() || std::filesystem::is_directory(path, ec))
                    return {false, "Drop .scene files only"};
                const std::string ext = path.extension().string();
                if (ext != ".scene" && ext != ".SCENE")
                    return {false, "Drop .scene assets only"};
            }
            return {true, {}};
        }
        const auto* bookmarkPayload = request.payload.TryGet<BookmarkDragPayload>();
        if (bookmarkPayload)
        {
            if (BookmarkToScenePaths(bookmarkPayload->bookmark).empty())
                return {false, "Bookmark must be a scene"};
            return {true, {}};
        }
        return {false, "No payload"};
    }

    void PerformDrop(const UI::Interaction::DropRequest& request) override
    {
        if (!m_OnDrop)
            return;
        const auto* assetPayload = request.payload.TryGet<AssetPathsDragPayload>();
        if (assetPayload && !assetPayload->paths.empty())
        {
            auto& assetManager = EngineCore::GetInstance().GetAssetManager();
            std::vector<std::string> relativePaths;
            for (const auto& absolutePath : assetPayload->paths)
            {
                std::string rel = TryMakeAssetRelativePathString(assetManager, absolutePath);
                if (!rel.empty())
                    relativePaths.push_back(std::move(rel));
            }
            if (!relativePaths.empty())
                m_OnDrop(relativePaths);
            return;
        }
        const auto* bookmarkPayload = request.payload.TryGet<BookmarkDragPayload>();
        if (bookmarkPayload)
        {
            std::vector<std::string> paths = BookmarkToScenePaths(bookmarkPayload->bookmark);
            if (!paths.empty())
                m_OnDrop(paths);
        }
    }

    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override
    {
        if (!state.Visible)
        {
            Overrides().Reset(Style::BorderWidth);
            Overrides().Reset(Style::BorderColor);
            return;
        }
        const uint32_t color = state.Allowed ? 0xE650C878u : 0xE6DC5050u;
        Overrides()
            .Set(Style::BorderWidth, Box4{2.0f, 2.0f, 2.0f, 2.0f})
            .Set(Style::BorderColor, BorderColorsTRBL{color, color, color, color});
    }

  private:
    OnDropFn m_OnDrop;
};

// Reads the scene paths back out of the rows and persists them. The rows are
// the model: a row's text field is the only place an edited path lives.
void SaveScenesFromRows(UIElement* rowsContainer, const std::string& platformName)
{
    if (!rowsContainer)
        return;
    std::vector<std::string> scenes;
    for (const auto& child : rowsContainer->GetChildren())
    {
        const auto& rowChildren = child->GetChildren();
        if (rowChildren.empty())
            continue;
        if (auto* field = dynamic_cast<TextField*>(rowChildren[0].get()))
            scenes.push_back(field->GetValue());
    }
    if (platformName.empty())
        SaveGlobalBuildScenes(scenes);
    else
        SaveBuildScenes(platformName, scenes);
}

void AppendSceneRow(UIElement* rowsContainer, const std::string& platformName,
                    const std::string& path)
{
    if (!rowsContainer)
        return;

    auto row = std::make_unique<UIElement>();
    row->AddClass("settings-build-scene-row");
    row->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    auto field = std::make_unique<TextField>();
    field->SetValue(path);
    field->AddClass("settings-row-field");
    field->SetOnValueChanged([rowsContainer, platformName](const std::string&)
    {
        SaveScenesFromRows(rowsContainer, platformName);
    });
    row->AddChild(std::move(field));

    auto remove = std::make_unique<Button>();
    remove->SetText("×");
    remove->AddClass("bookmark-remove");
    remove->RegisterEventHandler(kEventButtonClick, [rowsContainer, platformName](UIEvent& e)
    {
        UIElement& button = *e.CurrentTarget;
        UIElement* row = button.GetParent();
        if (!row || row->GetParent() != rowsContainer)
            return;
        rowsContainer->RemoveChild(row);
        SaveScenesFromRows(rowsContainer, platformName);
        rowsContainer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty |
                                 UIElement::ChildrenDirty);
    });
    row->AddChild(std::move(remove));

    rowsContainer->AddChild(std::move(row));
}

// The global scene list: drop zone plus one row per scene.
std::unique_ptr<UIElement> CreateGlobalSceneListBlock()
{
    auto block = MakeColumn(8.0f, 0.0f);
    block->AddClass("settings-build-global-fill");
    UIElement* blockPtr = block.get();

    auto dropTarget = std::make_unique<BuildSceneListDropTarget>(
        [blockPtr](const std::vector<std::string>& paths)
    {
        for (const std::string& path : paths)
            AppendSceneRow(blockPtr, {}, path);
        SaveScenesFromRows(blockPtr, {});
        blockPtr->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty |
                            UIElement::ChildrenDirty);
    });
    block->AddChild(std::move(dropTarget));

    for (const std::string& path : LoadGlobalBuildScenes())
        AppendSceneRow(blockPtr, {}, path);

    return block;
}

// A platform's scene list: the "use global" toggle plus the list it hides.
std::unique_ptr<UIElement> CreatePlatformSceneListBlock(const std::string& platformName)
{
    auto block = MakeColumn(8.0f, 0.0f);

    const bool useGlobal = LoadBuildPlatformUseGlobalScenes(platformName);

    auto list = MakeColumn(8.0f, 0.0f);
    list->Overrides().Set(Style::Visibility, !useGlobal);
    UIElement* listPtr = list.get();

    auto rows = std::make_unique<UIElement>();
    rows->Overrides().Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Column);
    UIElement* rowsPtr = rows.get();

    auto dropTarget = std::make_unique<BuildSceneListDropTarget>(
        [rowsPtr, platformName](const std::vector<std::string>& paths)
    {
        for (const std::string& path : paths)
            AppendSceneRow(rowsPtr, platformName, path);
        SaveScenesFromRows(rowsPtr, platformName);
        rowsPtr->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty |
                           UIElement::ChildrenDirty);
    });
    list->AddChild(std::move(dropTarget));
    list->AddChild(std::move(rows));

    for (const std::string& path : LoadBuildScenes(platformName))
        AppendSceneRow(rowsPtr, platformName, path);

    AppendStoreToggleRow(*block, "Use global scene list",
                         BuildPlatformPrefKey(platformName, "useGlobalScenes"), useGlobal,
                         "Ship the scenes from the global list instead of this platform's own.",
                         [listPtr](bool checked) { SetBlockVisibleDeferred(listPtr, !checked); });
    block->AddChild(std::move(list));
    return block;
}

// ---------------------------------------------------------------------------
// Application icon
// ---------------------------------------------------------------------------

bool IsSupportedBuildIconImage(const std::filesystem::path& path)
{
    if (path.empty())
        return false;
    return IsValidExtensionForAssetType(AssetType::Texture, path.extension().string());
}

std::filesystem::path ResolveBuildIconAbsolutePath(const std::string& relativePath)
{
    if (relativePath.empty())
        return {};
    const auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path candidate = assetManager.GetAssetRoot() / relativePath;
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec))
        return candidate;
    return {};
}

class BuildIconDropSlot final : public UIElement, public UI::Interaction::IDropTarget
{
  public:
    using OnIconPathChangedFn = std::function<void(const std::string& relativePath)>;

    BuildIconDropSlot(OnIconPathChangedFn onChanged, const std::string& initialRelativePath)
        : m_OnChanged(std::move(onChanged))
    {
        AddClass("settings-build-icon-slot");
        ApplyRelativePath(initialRelativePath);

        RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
        {
            if (e.Button != 0)
                return;
            if (!g_DragState.active || g_DragState.assetPath.empty())
                return;
            if (!IsSupportedBuildIconImage(g_DragState.assetPath))
                return;
            auto& assetManager = EngineCore::GetInstance().GetAssetManager();
            const std::string rel =
                TryMakeAssetRelativePathString(assetManager, g_DragState.assetPath);
            if (rel.empty())
                return;
            ApplyRelativePath(rel);
            if (m_OnChanged)
                m_OnChanged(rel);
            g_DragState.active = false;
            g_DragState.assetPath.clear();
            e.Stop();
        });
    }

    void ApplyRelativePath(const std::string& relativePath)
    {
        m_RelativePath = relativePath;
        std::filesystem::path previewPath;
        if (!relativePath.empty())
            previewPath = ResolveBuildIconAbsolutePath(relativePath);
        if (previewPath.empty())
            previewPath = ResolveDefaultEditorApplicationIconPath();

        if (previewPath.empty())
        {
            AddClass("settings-build-icon-slot-empty");
            UI::Layout::ClearBackgroundOverride(*this);
            return;
        }
        RemoveClass("settings-build-icon-slot-empty");
        UI::Layout::SetBackgroundPath(*this, previewPath.string());
    }

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override
    {
        return typeId == UI::Interaction::GetPayloadTypeId<AssetPathsDragPayload>();
    }

    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override
    {
        if (!ContainsPoint(x, y))
            return false;
        out.TargetId = 0;
        out.Location = UI::Interaction::DropLocation::OnItem;
        return true;
    }

    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override
    {
        const auto* assetPayload = request.payload.TryGet<AssetPathsDragPayload>();
        if (!assetPayload || assetPayload->paths.empty())
            return {false, "Drop an image file"};
        for (const auto& path : assetPayload->paths)
        {
            std::error_code ec;
            if (path.empty() || std::filesystem::is_directory(path, ec))
                return {false, "Drop an image file, not a folder"};
            if (!IsSupportedBuildIconImage(path))
                return {false, "Use PNG, JPG, WebP, or other texture formats"};
        }
        return {true, {}};
    }

    void PerformDrop(const UI::Interaction::DropRequest& request) override
    {
        const auto* assetPayload = request.payload.TryGet<AssetPathsDragPayload>();
        if (!assetPayload || assetPayload->paths.empty())
            return;
        auto& assetManager = EngineCore::GetInstance().GetAssetManager();
        for (const auto& absolutePath : assetPayload->paths)
        {
            if (!IsSupportedBuildIconImage(absolutePath))
                continue;
            const std::string rel = TryMakeAssetRelativePathString(assetManager, absolutePath);
            if (rel.empty())
                continue;
            ApplyRelativePath(rel);
            if (m_OnChanged)
                m_OnChanged(rel);
            return;
        }
    }

    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override
    {
        if (!state.Visible)
        {
            RemoveClass("drop-valid");
            RemoveClass("drop-invalid");
            return;
        }
        if (state.Allowed)
        {
            AddClass("drop-valid");
            RemoveClass("drop-invalid");
        }
        else
        {
            RemoveClass("drop-valid");
            AddClass("drop-invalid");
        }
    }

  private:
    OnIconPathChangedFn m_OnChanged;
    std::string m_RelativePath;
};

// Preview slot, editable project-relative path and a clear button, all bound to
// one icon preference key.
std::unique_ptr<UIElement> CreateIconEditorBlock(const std::string& iconPrefKey,
                                                 const std::string& initialRelativePath)
{
    auto block = MakeColumn(8.0f, 6.0f);

    auto row = std::make_unique<UIElement>();
    row->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::FlexStart)
        .Set(Style::Gap, StyleLength::Px(10.0f));

    auto pathField = std::make_unique<TextField>();
    pathField->SetValue(initialRelativePath);
    pathField->AddClass("settings-row-field");
    pathField->SetTooltip("Project-relative path to the application icon image.");
    TextField* pathFieldPtr = pathField.get();

    auto slot = std::make_unique<BuildIconDropSlot>(
        [iconPrefKey, pathFieldPtr](const std::string& rel)
    {
        SaveBuildStringSetting(iconPrefKey, rel);
        if (pathFieldPtr)
            pathFieldPtr->SetValue(rel);
    },
        initialRelativePath);
    BuildIconDropSlot* slotPtr = slot.get();
    row->AddChild(std::move(slot));

    auto fields = std::make_unique<UIElement>();
    fields->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(0.0f));

    // Focus-out fires on an untouched field too, so an unchanged path must not
    // write — opening the page is not an edit.
    auto committed = std::make_shared<std::string>(initialRelativePath);
    auto commitIcon = [iconPrefKey, slotPtr, committed](const std::string& value)
    {
        if (*committed == value)
            return;
        *committed = value;
        SaveBuildStringSetting(iconPrefKey, value);
        if (slotPtr)
            slotPtr->ApplyRelativePath(value);
    };
    pathFieldPtr->SetOnValueChanged(commitIcon);
    pathFieldPtr->RegisterEventHandler(kEventFocusOut, [pathFieldPtr, commitIcon](UIEvent&)
    {
        commitIcon(pathFieldPtr->GetValue());
    });

    auto pathRow = std::make_unique<UIElement>();
    pathRow->AddClass("settings-build-scene-row");
    pathRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));
    pathRow->AddChild(std::move(pathField));

    auto clear = std::make_unique<Button>();
    clear->SetText("×");
    clear->AddClass("bookmark-remove");
    clear->SetTooltip("Clear icon override (use Editor default icon).");
    clear->RegisterEventHandler(kEventButtonClick, [commitIcon, pathFieldPtr](UIEvent&)
    {
        if (pathFieldPtr)
            pathFieldPtr->SetValue("");
        commitIcon({});
    });
    pathRow->AddChild(std::move(clear));
    fields->AddChild(std::move(pathRow));

    auto hint = std::make_unique<EditorUI::CollapsibleInfoCard>(
        "Drag an image from the Assets panel onto the preview. When empty, the Editor "
        "application icon is used.");
    hint->Overrides().Set(Style::PaddingLeft, StyleLength::Px(0.0f));
    fields->AddChild(std::move(hint));

    row->AddChild(std::move(fields));
    block->AddChild(std::move(row));
    return block;
}

// A platform's icon: the "use global" toggle plus the editor it hides.
std::unique_ptr<UIElement> CreatePlatformIconBlock(const std::string& platformName)
{
    auto block = MakeColumn(0.0f, 0.0f);

    const bool useGlobal = LoadBuildPlatformUseGlobalIcon(platformName);
    auto editor = CreateIconEditorBlock(BuildPlatformPrefKey(platformName, "icon"),
                                        LoadBuildPlatformIcon(platformName));
    editor->Overrides().Set(Style::Visibility, !useGlobal);
    UIElement* editorPtr = editor.get();

    AppendStoreToggleRow(*block, "Use Global App Icon",
                         BuildPlatformPrefKey(platformName, "useGlobalIcon"), useGlobal,
                         "Use global application icon",
                         [editorPtr](bool checked) { SetBlockVisibleDeferred(editorPtr, !checked); });
    block->AddChild(std::move(editor));
    return block;
}

// ---------------------------------------------------------------------------
// Per-platform render pipeline
// ---------------------------------------------------------------------------

// The "use global" toggle plus the per-platform pipeline dropdown it hides.
std::unique_ptr<UIElement> CreatePlatformRenderPipelineBlock(const std::string& platformName)
{
    auto block = MakeColumn(0.0f, 0.0f);

    const bool useGlobal = LoadBuildPlatformUseGlobalRenderPipeline(platformName);
    const std::string selected = useGlobal ? ResolveBuildGlobalRenderPipeline()
                                           : ResolveBuildRenderPipelineForPlatform(platformName);

    auto pipeline = MakeColumn(0.0f, 0.0f);
    pipeline->Overrides().Set(Style::Visibility, !useGlobal);
    UIElement* pipelinePtr = pipeline.get();

    auto row = MakeSettingsRow();
    row->AddChild(MakeRowLabel("Render pipeline"));

    std::vector<Dropdown::Option> options;
    for (const auto& option : CollectRenderPipelineOptions(selected))
        options.push_back(Dropdown::Option{option.Value, option.Label});

    auto dropdown = std::make_unique<Dropdown>();
    dropdown->AddClass("settings-row-field");
    dropdown->SetOptions(options, 0);
    dropdown->SetSelectedValue(selected);
    dropdown->SetOnValueChanged([platformName](const std::string& value)
    {
        SaveBuildPlatformRenderPipeline(platformName, value);
    });
    row->AddChild(std::move(dropdown));
    pipeline->AddChild(std::move(row));

    auto useGlobalRow = MakeSettingsRow();
    useGlobalRow->Overrides().Set(Style::MarginTop, StyleLength::Px(8.0f));
    useGlobalRow->AddChild(MakeRowLabel("Use global render pipeline"));
    auto toggle = std::make_unique<Toggle>();
    toggle->SetChecked(useGlobal);
    toggle->SetOnValueChanged([platformName, pipelinePtr](bool checked)
    {
        SaveBuildPlatformUseGlobalRenderPipeline(platformName, checked);
        SetBlockVisibleDeferred(pipelinePtr, !checked);
    });
    useGlobalRow->AddChild(std::move(toggle));

    block->AddChild(std::move(useGlobalRow));
    block->AddChild(std::move(pipeline));
    return block;
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

SettingsFieldDescriptor MakeStoreStringField(std::string label, std::string prefKey,
                                             std::string defaultValue, std::string tooltip,
                                             std::string searchKeywords)
{
    SettingsFieldDescriptor field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    field.SearchKeywords = std::move(searchKeywords);

    SettingsFieldDescriptor::StringField control;
    control.DefaultValue = defaultValue;
    control.Get = [prefKey, defaultValue]() { return LoadBuildStringSetting(prefKey, defaultValue); };
    control.Set = [prefKey](const std::string& value) { SaveBuildStringSetting(prefKey, value); };
    field.Control = std::move(control);
    return field;
}

SettingsFieldDescriptor MakeCustomField(std::string label, std::string searchKeywords,
                                        std::function<std::unique_ptr<UIElement>()> create)
{
    SettingsFieldDescriptor field;
    field.Label = std::move(label);
    field.SearchKeywords = std::move(searchKeywords);
    SettingsFieldDescriptor::CustomField control;
    control.CreateRow = std::move(create);
    field.Control = std::move(control);
    return field;
}

void AppendBuildPageFields(SettingsCategoryDescriptor& build, const BuildSettingsPageHooks& hooks)
{
    {
        SettingsFieldDescriptor open;
        open.Tooltip = "Open the Build panel for the current build configuration.";
        open.SearchKeywords = "build panel window open package";
        SettingsFieldDescriptor::ButtonField control;
        control.ButtonText = "Open Build Window";
        control.OnClick = [hooks]()
        {
            if (hooks.OpenBuildPanel)
                hooks.OpenBuildPanel();
        };
        // Already front-most: the row would only take the user where they are.
        control.IsVisible = [hooks]()
        { return !(hooks.IsBuildPanelActive && hooks.IsBuildPanelActive()); };
        open.Control = std::move(control);
        build.Fields.push_back(std::move(open));
    }

    {
        SettingsFieldDescriptor name = MakeStoreStringField(
            "Name", kBuildGameNamePrefKey, {},
            "Name of the shipped game. Used for the executable/bundle name, the bundle "
            "identifier, and game.config. Defaults to the project folder name.",
            "build game name executable bundle identifier gameconfig");
        name.SectionHeader = "Game name";
        name.SectionDescription =
            "Name of the shipped game. Used for the executable/bundle name, the bundle "
            "identifier, and game.config. Defaults to the project folder name.";
        build.Fields.push_back(std::move(name));
    }

    {
        SettingsFieldDescriptor icon = MakeCustomField(
            "Application icon", "build application icon image png bundle",
            []() { return CreateIconEditorBlock(kGlobalBuildIconPrefKey, LoadBuildGlobalIcon()); });
        icon.SectionHeader = "Application icon";
        icon.SectionDescription =
            "Default icon for all platforms. Per-platform overrides can use a custom icon instead.";
        build.Fields.push_back(std::move(icon));
    }

    {
        // Dropdown labels double as the stored value, so the game.config token
        // mapping happens here rather than in the panel.
        SettingsFieldDescriptor windowMode;
        windowMode.Label = "Window mode";
        windowMode.SectionHeader = "Player";
        windowMode.SectionDescription =
            "Default window mode for exported Player builds (written to game.config).";
        windowMode.SearchKeywords = "build player window mode fullscreen borderless exclusive windowed";
        SettingsFieldDescriptor::DropdownField control;
        control.OptionsProvider = []()
        {
            return std::vector<SettingsFieldDescriptor::DropdownField::Option>{
                {"windowed", "Windowed"},
                {"borderless", "Fullscreen (borderless)"},
                {"exclusive", "Exclusive fullscreen"},
            };
        };
        control.DefaultValue = WindowModeToString(WindowMode::Windowed);
        control.Get = []() { return std::string(WindowModeToString(LoadBuildPlayerWindowMode())); };
        control.Set = [](const std::string& value)
        {
            WindowMode parsed = WindowMode::Windowed;
            if (TryParseWindowMode(value, parsed))
                SaveBuildPlayerWindowMode(parsed);
        };
        windowMode.Control = std::move(control);
        build.Fields.push_back(std::move(windowMode));
    }

    {
        SettingsFieldDescriptor pipeline;
        pipeline.Label = "Global pipeline";
        pipeline.SectionHeader = "Render pipeline";
        pipeline.SectionDescription =
            "Pipeline written to game.config for builds. Platforms can use this global pipeline "
            "or override per platform.";
        pipeline.SearchKeywords = "build render pipeline rendergraph forward clustered gameconfig";
        SettingsFieldDescriptor::DropdownField control;
        control.OptionsProvider = []()
        { return CollectRenderPipelineOptions(ResolveBuildGlobalRenderPipeline()); };
        control.DefaultValue = kDefaultBuildRenderPipelinePath;
        control.Get = []() { return ResolveBuildGlobalRenderPipeline(); };
        control.Set = [](const std::string& value) { SaveBuildGlobalRenderPipeline(value); };
        pipeline.Control = std::move(control);
        build.Fields.push_back(std::move(pipeline));
    }

    {
        SettingsFieldDescriptor scenes =
            MakeCustomField("Global scene list", "build global scene list scenes package",
                            []() { return CreateGlobalSceneListBlock(); });
        scenes.SectionHeader = "Global scene list";
        scenes.SectionDescription =
            "Scenes included when a platform has \"Use global scene list\" enabled.";
        build.Fields.push_back(std::move(scenes));
    }
}

// A store-backed toggle row. Its Set guards on the stored value: the row fires
// its callback once with the value it was seeded from, and writing that back
// would rewrite the project file just for opening the page.
SettingsFieldDescriptor MakeStoreToggleField(std::string label, std::string prefKey,
                                             bool defaultValue, std::string tooltip,
                                             std::string searchKeywords)
{
    SettingsFieldDescriptor field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    field.SearchKeywords = std::move(searchKeywords);

    SettingsFieldDescriptor::ToggleField control;
    control.DefaultValue = defaultValue;
    control.Get = [prefKey, defaultValue]() { return LoadBuildBoolSetting(prefKey, defaultValue); };
    control.Set = [prefKey, defaultValue](bool value)
    {
        if (LoadBuildBoolSetting(prefKey, defaultValue) == value)
            return;
        SaveBuildBoolSetting(prefKey, value);
    };
    field.Control = std::move(control);
    return field;
}

SettingsFieldDescriptor MakeStorePathField(std::string label,
                                           SettingsFieldDescriptor::PathField::Kind kind,
                                           std::string prefKey, std::string defaultValue,
                                           std::string tooltip, std::string searchKeywords)
{
    SettingsFieldDescriptor field;
    field.Label = std::move(label);
    field.Tooltip = std::move(tooltip);
    field.SearchKeywords = std::move(searchKeywords);

    SettingsFieldDescriptor::PathField control;
    control.PathKind = kind;
    control.DefaultValue = defaultValue;
    control.Get = [prefKey, defaultValue]() { return LoadBuildStringSetting(prefKey, defaultValue); };
    control.Set = [prefKey](const std::string& value) { SaveBuildStringSetting(prefKey, value); };
    field.Control = std::move(control);
    return field;
}

void AppendSteamInstallFields(SettingsCategoryDescriptor& platform, const std::string& platformName)
{
    const std::string search = "build steam deck install ssh deploy remote";
    const auto key = [&platformName](const char* leaf)
    { return BuildPlatformPrefKey(platformName, leaf); };

    SettingsFieldDescriptor autoInstall = MakeStoreToggleField(
        "Auto install after build", key("autoInstallAfterBuild"), false,
        "Copy the Steam Deck build to the configured Steam Deck host after a successful build.",
        search);
    autoInstall.SectionHeader = "Steam Deck install";
    platform.Fields.push_back(std::move(autoInstall));

    platform.Fields.push_back(MakeStoreStringField(
        "Deck host", key("installSshHost"), {},
        "SSH host name or IP address of the Steam Deck.", search));
    platform.Fields.push_back(MakeStoreStringField(
        "SSH user", key("installSshUser"), "deck",
        "SSH username used to connect to the Steam Deck.", search));
    platform.Fields.push_back(MakeStorePathField(
        "SSH key", SettingsFieldDescriptor::PathField::Kind::File, key("installSshKeyPath"), {},
        "Optional private key path used for Steam Deck SSH.", search));
    platform.Fields.push_back(MakeStoreStringField(
        "Remote folder", key("installRemoteDirectory"),
        "/home/deck/devkit-game/GameEnginePlayer_Linux", "Destination folder on the Steam Deck.",
        search));
    platform.Fields.push_back(MakeStoreToggleField(
        "Register Steam shortcut", key("installSteamShortcut"), true,
        "Create or update a Steam shortcut for the installed build.", search));
}

void AppendWebExportFields(SettingsCategoryDescriptor& platform, const std::string& platformName)
{
    SettingsFieldDescriptor templateDir = MakeStorePathField(
        "Player template", SettingsFieldDescriptor::PathField::Kind::Directory,
        BuildPlatformPrefKey(platformName, "playerTemplate"), {},
        "Directory holding WebPlayer.html, WebPlayer.js and WebPlayer.wasm from a wasm Player "
        "build (cmake --build --preset wasm-debug --target WebPlayer). Leave empty to use the "
        "engine tree's build/wasm-release/bin or build/wasm-debug/bin.",
        "build web export player template wasm emscripten browser");
    templateDir.SectionHeader = "Web export";
    platform.Fields.push_back(std::move(templateDir));
}

SettingsCategoryDescriptor MakePlatformCategory(const BuildPlatformInfo& info,
                                                const BuildSettingsPageHooks& hooks)
{
    const std::string platformName(info.Name);

    SettingsCategoryDescriptor platform;
    platform.CategoryId = PlatformCategoryId(platformName);
    platform.Title = std::string(info.DisplayName);
    platform.Group = SettingsCategoryGroup::ProjectSettings;
    platform.ParentCategoryId = kBuildCategoryId;
    platform.TreeRowClass = std::string(info.SettingsTreeRowClass);
    platform.SearchKeywords = "build platform " + platformName;

    {
        const std::string versionKey = BuildPlatformPrefKey(platformName, "version");
        // Not BuildPlatformInfo::DefaultVersion: that column feeds the Build
        // panel and holds placeholders ("."), which are not a version.
        const std::string defaultVersion = "1";
        SettingsFieldDescriptor version;
        version.Label = "Version";
        version.SearchKeywords = "build version platform " + platformName;
        SettingsFieldDescriptor::StringField control;
        control.DefaultValue = defaultVersion;
        control.Get = [versionKey, defaultVersion]()
        { return LoadBuildStringSetting(versionKey, defaultVersion); };
        control.Set = [versionKey, hooks](const std::string& value)
        {
            SaveBuildStringSetting(versionKey, value);
            // Notifying rebuilds the Build panel's rows, which must not happen
            // while this text field is still dispatching.
            if (hooks.Defer)
                hooks.Defer([]() { NotifyBuildVersionChanged(); });
            else
                NotifyBuildVersionChanged();
        };
        version.Control = std::move(control);
        platform.Fields.push_back(std::move(version));
    }

    platform.Fields.push_back(MakeStoreStringField(
        "Build folder", BuildPlatformPrefKey(platformName, "outputDir"),
        std::string(kDefaultBuildFolderName),
        "Folder the packaged build is written to, relative to the workspace.",
        "build output folder directory package " + platformName));

    platform.Fields.push_back(MakeStoreStringField(
        "Configuration", BuildPlatformPrefKey(platformName, "buildConfig"),
        std::string(DefaultPlayerBuildConfig()),
        "Player build configuration compiled for this platform.",
        "build configuration debug release player " + platformName));

    platform.Fields.push_back(
        MakeCustomField("Render pipeline", "build render pipeline platform override " + platformName,
                        [platformName]() { return CreatePlatformRenderPipelineBlock(platformName); }));

    {
        SettingsFieldDescriptor icon = MakeCustomField(
            "Application icon", "build application icon platform override " + platformName,
            [platformName]() { return CreatePlatformIconBlock(platformName); });
        icon.SectionHeader = "Application icon";
        platform.Fields.push_back(std::move(icon));
    }

    if (platformName == "Steam")
        AppendSteamInstallFields(platform, platformName);
    if (platformName == kWebBuildPlatformName)
        AppendWebExportFields(platform, platformName);

    {
        SettingsFieldDescriptor scenes = MakeCustomField(
            "Scenes to build", "build scenes platform list " + platformName,
            [platformName]() { return CreatePlatformSceneListBlock(platformName); });
        scenes.SectionHeader = "Scenes";
        platform.Fields.push_back(std::move(scenes));
    }

    return platform;
}

} // namespace

void RegisterBuildSettingsCategories(BuildSettingsPageHooks hooks)
{
    SettingsCategoryDescriptor build;
    build.CategoryId = kBuildCategoryId;
    build.Title = "Build";
    build.Group = SettingsCategoryGroup::ProjectSettings;
    build.TreeRowClass = "build-row";
    build.SearchKeywords = "build platform version package export";
    AppendBuildPageFields(build, hooks);
    EditorSettingsRegistry::Get().RegisterCategory(std::move(build));

    // Registration order is presentation order for sub-pages, and
    // GetBuildPlatforms() is already in presentation order.
    for (const BuildPlatformInfo& info : GetBuildPlatforms())
    {
        if (!info.ShowInSettingsTree)
            continue;
        EditorSettingsRegistry::Get().RegisterCategory(MakePlatformCategory(info, hooks));
    }
}

} // namespace Editor
} // namespace GameEngine
