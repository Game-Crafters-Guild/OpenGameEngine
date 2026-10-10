#include "VersionControl/Ui/InspectorVcsController.h"

#include "Components/SceneEntityTag.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/World.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Vcs/VcsStatusUi.h"
#include "EditorChangeNotifications.h"
#include "EditorContext.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Logger/Logger.h"
#include "Panels/InspectorPanel.h"
#include "Platform/Clipboard.h"
#include "Platform/ContextMenu.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/WorldSnapshotCommand.h"
#include "UI/Controls/Label.h"
#include "UI/EditorIcons.h"
#include "UI/InspectorSection.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"
#include "VersionControl/EditorVersionControlService.h"
#include "VersionControl/SceneDiffLiveOverlay.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace GameEngine::Editor
{
namespace
{
std::string NormalizeName(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const char ch : text)
        if (std::isalnum(static_cast<unsigned char>(ch)))
            result.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return result;
}

std::string NormalizeComponentName(std::string_view text)
{
    // Reflected canonical names are fully qualified
    // (for example, "GameEngine::Components::Light"), while scene property
    // keys use the serialized component name ("Light.intensity"). Compare the
    // unqualified type name so Inspector sections can resolve their scene
    // properties across compilers and registration paths.
    if (const size_t separator = text.rfind("::");
        separator != std::string_view::npos)
        text.remove_prefix(separator + 2);
    return NormalizeName(text);
}

Label* FindRowLabel(UIElement* element)
{
    if (!element)
        return nullptr;
    if (auto* label = dynamic_cast<Label*>(element);
        label && label->HasClass("inspector-label"))
        return label;
    for (const auto& child : element->GetChildren())
        if (Label* label = FindRowLabel(child.get()))
            return label;
    return nullptr;
}

void CollectRows(UIElement* element, std::vector<UIElement*>& rows)
{
    if (!element)
        return;
    if (element->HasClass("inspector-row"))
        rows.push_back(element);
    for (const auto& child : element->GetChildren())
        CollectRows(child.get(), rows);
}

bool IsContextMenuButton(int button)
{
    return button == 1 || button == 2;
}

const char* DiffClass(SceneDiffState state)
{
    switch (state)
    {
        case SceneDiffState::Added: return "inspector-vcs-diff-added";
        case SceneDiffState::Removed: return "inspector-vcs-diff-removed";
        case SceneDiffState::Modified: return "inspector-vcs-diff-modified";
        case SceneDiffState::Unchanged: return nullptr;
    }
    return nullptr;
}

bool ElementContains(const UIElement* root, const UIElement* target)
{
    if (!root || !target)
        return false;
    if (root == target)
        return true;
    for (const auto& child : root->GetChildren())
        if (ElementContains(child.get(), target))
            return true;
    return false;
}

void RemoveDiffDots(UIElement* element)
{
    if (!element)
        return;
    std::vector<UIElement*> remove;
    for (const auto& child : element->GetChildren())
    {
        if (child->HasClass("inspector-vcs-diff-dot"))
            remove.push_back(child.get());
        else
            RemoveDiffDots(child.get());
    }
    for (UIElement* child : remove)
        element->RemoveChild(child);
}

void RemoveDiffDotFromRow(UIElement* row)
{
    if (!row)
        return;
    std::vector<UIElement*> remove;
    for (const auto& child : row->GetChildren())
        if (child->HasClass("inspector-vcs-diff-dot"))
            remove.push_back(child.get());
    for (UIElement* child : remove)
        row->RemoveChild(child);
}

std::string DiffTooltip(std::string_view providerName,
                        const ScenePropertyDiff& property)
{
    std::ostringstream tooltip;
    if (!providerName.empty())
        tooltip << providerName << " · ";
    tooltip << SceneDiffStateLabel(property.State) << '\n' << property.Key;
    if (!property.OriginalValue.empty())
        tooltip << "\nBaseline: " << property.OriginalValue;
    if (!property.CurrentValue.empty())
        tooltip << "\nCurrent: " << property.CurrentValue;
    return tooltip.str();
}

void InsertDiffDot(UIElement* row, Label* label,
                   const ScenePropertyDiff& property,
                   std::string_view providerName,
                   std::function<void(float, float)> onContextMenu)
{
    if (!row || !label)
        return;
    auto dot = std::make_unique<UIElement>();
    dot->AddClass("inspector-vcs-diff-dot");
    if (const char* cssClass = DiffClass(property.State))
        dot->AddClass(cssClass);
    dot->Overrides().Set(Style::BackgroundTint,
                         SceneDiffStateColorArgb(property.State));

    dot->SetTooltip(DiffTooltip(providerName, property));
    // Pop on release, not press: the macOS NSMenu tracking loop has to start
    // from a clean button-up state or it swallows the press that opened it and
    // the menu only appears on a second click. The press is still consumed here
    // so it cannot reach the row underneath.
    dot->RegisterEventHandler(
        kEventMouseDown,
        [](UIEvent& event)
        {
            if (IsContextMenuButton(event.Button))
                event.Stop();
        });
    dot->RegisterEventHandler(
        kEventMouseUp,
        [onContextMenu = std::move(onContextMenu)](UIEvent& event)
        {
            if (IsContextMenuButton(event.Button) && onContextMenu)
            {
                onContextMenu(event.X, event.Y);
                event.Stop();
            }
        });

    size_t insertIndex = row->GetChildren().size();
    for (size_t i = 0; i < row->GetChildren().size(); ++i)
        if (ElementContains(row->GetChildren()[i].get(), label))
        {
            insertIndex = i + 1;
            break;
        }
    row->InsertChild(insertIndex, std::move(dot));
}

void InsertPlaceholder(UIElement* row, Label* label)
{
    if (!row || !label)
        return;
    auto dot = std::make_unique<UIElement>();
    dot->AddClass("inspector-vcs-diff-dot");
    dot->AddClass("inspector-vcs-diff-dot-placeholder");

    size_t insertIndex = row->GetChildren().size();
    for (size_t i = 0; i < row->GetChildren().size(); ++i)
        if (ElementContains(row->GetChildren()[i].get(), label))
        {
            insertIndex = i + 1;
            break;
        }
    row->InsertChild(insertIndex, std::move(dot));
}

void ClearDiffClasses(UIElement* element)
{
    if (!element)
        return;
    element->RemoveClass("inspector-vcs-diff-added");
    element->RemoveClass("inspector-vcs-diff-removed");
    element->RemoveClass("inspector-vcs-diff-modified");
    element->RemoveClass("inspector-vcs-diff-section");
    for (const auto& child : element->GetChildren())
        ClearDiffClasses(child.get());
}
} // namespace

InspectorVcsController::InspectorVcsController(InspectorPanel& panel)
    : m_Panel(panel)
{
}

InspectorVcsController::~InspectorVcsController()
{
    DetachService();
}

void InspectorVcsController::DetachService()
{
    // Unsubscribing does not join a callback already running on the status
    // thread (Broadcast copies the list and invokes outside the lock). A running
    // callback only touches its own copy of the coalesced post, and Cancel keeps
    // any refresh it queued from running.
    m_StatusRefresh.Cancel();
    m_VcsSubscription.Reset();
}

void InspectorVcsController::SetContext(const EditorContext* context)
{
    DetachService();
    if (!context || context->UIReplayActive || !context->VcsService)
        return;
    // Broadcast arrives on the provider's status thread and fires once per poll
    // whether or not anything changed, so the callback must stay cheap and must
    // not outlive this controller. It marshals to the UI thread and refreshes
    // only the markers — a full ShowEntity rebuild here would destroy focus and
    // in-progress edits on every poll.
    // The listener runs on the provider's status thread and does nothing but request: the
    // provider registry is main-thread-only (unlocked; package module reloads mutate it), so
    // even the has-anything-changed check waits for the UI thread. An idle poll then costs one
    // posted no-op.
    m_StatusRefresh = UI::UiCoalescedPost(m_Panel.GetPostHandle(), [this]()
    {
        // The broadcast fires once per poll whether or not anything changed; only a real
        // status transition invalidates and refreshes.
        if (!InvalidateVersionControlledSceneDiffsWhoseStatusChanged())
            return;
        ApplyDecorations();
    });
    m_VcsSubscription = context->VcsService->AddListener([refresh = m_StatusRefresh]() { refresh.Request(); });
}

void InspectorVcsController::SetSceneDiffProvider(
    std::function<std::vector<SceneObjectDiff>()> provider)
{
    m_SceneDiffProvider = std::move(provider);
}

void InspectorVcsController::ApplyDecorations()
{
    RemoveDiffDots(m_Panel.m_TopRoot);
    RemoveDiffDots(m_Panel.m_ContentRoot);
    ClearDiffClasses(m_Panel.m_TopRoot);
    ClearDiffClasses(m_Panel.m_ContentRoot);

    if (!AreVcsSceneDiffIndicatorsVisible())
        return;

    if (!m_SceneDiffProvider || !m_Panel.m_World ||
        !m_Panel.m_Entity.IsValid() ||
        !m_Panel.m_World->IsValid(m_Panel.m_Entity))
        return;
    const auto* tag =
        m_Panel.m_World->GetComponent<Components::SceneEntityTag>(
            m_Panel.m_Entity);
    if (!tag || tag->value[0] == '\0')
        return;

    const std::string stableKey = std::string("entity:") + std::string(tag->View());
    const std::vector<SceneObjectDiff> sceneDiff = m_SceneDiffProvider();
    const auto objectIt = std::find_if(
        sceneDiff.begin(), sceneDiff.end(),
        [&stableKey](const SceneObjectDiff& object)
        {
            return object.StableKey == stableKey;
        });
    if (objectIt == sceneDiff.end())
        return;
    const SceneObjectDiff object = OverlaySceneDiffWithLiveValues(
        *objectIt, *m_Panel.m_World, m_Panel.m_Entity);
    if (object.State == SceneDiffState::Unchanged)
        return;

    // Only entities that actually carry a diff reserve the marker column, so an
    // unchanged entity costs no per-row element. Rows within one entity stay
    // aligned, which is the shift the placeholders exist to prevent.
    std::vector<UIElement*> allRows;
    CollectRows(m_Panel.m_ContentRoot, allRows);
    for (UIElement* row : allRows)
        if (Label* label = FindRowLabel(row))
            InsertPlaceholder(row, label);

    const auto nameIt = std::find_if(
        object.Properties.begin(), object.Properties.end(),
        [](const ScenePropertyDiff& property)
        {
            return property.Key == "Name.value";
        });
    if (nameIt != object.Properties.end() &&
        nameIt->State != SceneDiffState::Unchanged && m_Panel.m_TopRoot)
        for (const auto& child : m_Panel.m_TopRoot->GetChildren())
            if (child->HasClass("inspector-header-container"))
                if (const char* cssClass = DiffClass(nameIt->State))
                    child->AddClass(cssClass);

    std::string providerName;
    EditorVcsProviderDescriptor provider;
    if (EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider))
        providerName = provider.DisplayName;

    for (const auto& [section, typeId] : m_Panel.m_SectionTypeIds)
    {
        std::vector<std::string> componentNames;
        if (const auto* info = ECS::ComponentRegistry::GetComponentInfo(typeId))
            componentNames.push_back(NormalizeComponentName(info->Name));
        if (const std::string_view canonical =
                ECS::ComponentFieldRegistry::GetCanonicalName(typeId);
            !canonical.empty())
            componentNames.push_back(NormalizeComponentName(canonical));
        struct ChangedField
        {
            std::string Name;
            ScenePropertyDiff Property;
        };
        std::vector<ChangedField> changedFields;
        for (const ScenePropertyDiff& property : object.Properties)
        {
            if (property.State == SceneDiffState::Unchanged)
                continue;
            const size_t dot = property.Key.find('.');
            if (dot == std::string::npos)
                continue;
            const std::string component =
                NormalizeName(property.Key.substr(0, dot));
            if (std::find(componentNames.begin(), componentNames.end(),
                          component) == componentNames.end())
                continue;
            changedFields.push_back(
                {NormalizeName(property.Key.substr(dot + 1)), property});
        }
        if (changedFields.empty())
            continue;

        section->AddClass("inspector-vcs-diff-section");
        section->AddClass(DiffClass(object.State));
        std::vector<UIElement*> rows;
        CollectRows(section->GetContentRoot(), rows);
        for (UIElement* row : rows)
        {
            Label* label = FindRowLabel(row);
            if (!label)
                continue;
            const std::string rowName = NormalizeName(label->GetText());
            const auto fieldIt = std::find_if(
                changedFields.begin(), changedFields.end(),
                [&rowName](const ChangedField& field)
                {
                    return field.Name == rowName;
                });
            if (fieldIt == changedFields.end())
                continue;
            RemoveDiffDotFromRow(row);
            InsertDiffDot(
                row, label, fieldIt->Property, providerName,
                [this, property = fieldIt->Property](float x, float y)
                {
                    ShowPropertyContextMenu(property, x, y);
                });
        }
    }
}

void InspectorVcsController::ShowPropertyContextMenu(
    const ScenePropertyDiff& property, float x, float y)
{
    if (!m_Panel.m_Window)
        return;
    if (!m_ContextMenu)
        m_ContextMenu = CreateContextMenu();
    if (!m_ContextMenu)
        return;

    constexpr uint32_t kRevertProperty = 1;
    constexpr uint32_t kCopyBaseline = 2;
    constexpr uint32_t kCopyCurrent = 3;
    m_ContextMenu->Clear();
    m_ContextMenu->SetCommandHandler(
        [this, property](uint32_t command)
        {
            if (command == kRevertProperty)
                RevertProperty(property);
            else if (command == kCopyBaseline)
                Platform::SetClipboardText(property.OriginalValue.c_str());
            else if (command == kCopyCurrent)
                Platform::SetClipboardText(property.CurrentValue.c_str());
        });

    ContextMenuBuilder builder;
    builder.AddItem(
        "Revert Property to Baseline", kRevertProperty,
        property.OriginalValue.empty() ? MenuItemFlag_Disabled
                                       : MenuItemFlag_None,
        0, EditorIcons::kReset);
    builder.AddItem(
        "Copy Baseline Value", kCopyBaseline,
        property.OriginalValue.empty() ? MenuItemFlag_Disabled
                                       : MenuItemFlag_None,
        10, EditorIcons::kCopy);
    builder.AddItem(
        "Copy Current Value", kCopyCurrent,
        property.CurrentValue.empty() ? MenuItemFlag_Disabled
                                      : MenuItemFlag_None,
        20, EditorIcons::kCopy);
    builder.Build(m_ContextMenu.get());
    m_ContextMenu->Show(
        m_Panel.m_Window, static_cast<int>(x), static_cast<int>(y));
}

void InspectorVcsController::RevertProperty(const ScenePropertyDiff& property)
{
    if (!m_Panel.m_World || !m_Panel.m_Entity.IsValid() ||
        !m_Panel.m_World->IsValid(m_Panel.m_Entity) ||
        property.OriginalValue.empty())
        return;
    const size_t dot = property.Key.find('.');
    if (dot == std::string::npos)
        return;
    const std::string component = property.Key.substr(0, dot);
    const std::string field =
        SceneSchemaFieldName(std::string_view(property.Key).substr(dot + 1));
    const Scene::ISceneComponentSchema* schema =
        Scene::SceneSchemaRegistry::Find(component);
    if (!schema)
        return;

    const std::vector<uint8_t> before = m_Panel.m_World->SerializeWorld();
    Scene::SceneLoadContext loadContext{};
    loadContext.TargetWorld = m_Panel.m_World;
    std::string error;
    if (!schema->ApplyProperty(
            *m_Panel.m_World, m_Panel.m_Entity, loadContext, field,
            property.OriginalValue, &error))
    {
        Logger::Log::Warning(
            "Inspector scene diff: failed to revert '{}': {}",
            property.Key, error);
        return;
    }
    m_Panel.m_World->ProcessCommands();
    const std::vector<uint8_t> after = m_Panel.m_World->SerializeWorld();
    if (m_Panel.m_Undo && before != after)
        m_Panel.m_Undo->CommitAlreadyApplied(
            std::make_unique<WorldSnapshotCommand>(
                std::string("Revert ") + property.Key + " to VCS baseline",
                m_Panel.m_World, m_Panel.m_ChangeNotifications,
                before, after));
    if (m_Panel.m_ChangeNotifications)
    {
        EditorChangeNotifications::WorldStructureChangedEvent event{};
        event.world = m_Panel.m_World;
        event.kind = EditorChangeNotifications::ChangeKind::Commit;
        m_Panel.m_ChangeNotifications->NotifyWorldStructureChanged(event);
    }
    m_Panel.ShowEntity(
        m_Panel.m_World, m_Panel.m_Entity, false, false);
}

} // namespace GameEngine::Editor
