#include "UI/EntityField.h"

#include "Components/Name.h"
#include "ECS/Entity.h" // full World definition
#include "Editor/DragDropPayloads.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Hierarchy/HierarchyEntityDecode.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Interaction/Payload.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{

namespace
{
std::string ToLowerCopy(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// True if the entity has every component in `required` (empty -> always true).
bool EntityHasAll(ECS::World* world, ECS::EntityHandle e,
                  const std::vector<ECS::ComponentTypeId>& required)
{
    for (ECS::ComponentTypeId tid : required)
        if (!world->HasComponent(e, tid))
            return false;
    return true;
}

// Lists alive entities (optionally restricted to those with required components),
// matched by a case-insensitive substring of their display name.
class EntitySearchProvider final : public ISearchProvider
{
public:
    EntitySearchProvider(ECS::World* world, std::vector<ECS::ComponentTypeId> required)
        : m_World(world), m_Required(std::move(required)) {}

    void BeginSearch(const std::string& query, ResultSink sink) override
    {
        std::vector<SearchResultItem> results;
        if (m_World)
        {
            std::vector<ECS::EntityHandle> all;
            m_World->GetAliveEntitiesSnapshot(all);
            const std::string q = ToLowerCopy(query);
            for (ECS::EntityHandle e : all)
            {
                if (!EntityHasAll(m_World, e, m_Required))
                    continue;
                std::string name = Editor::EntityDisplayName(*m_World, e);
                if (!q.empty() && ToLowerCopy(name).find(q) == std::string::npos)
                    continue;

                // A named entity also shows its id, so two entities of one name can be told apart.
                const auto* nameComponent = m_World->GetComponent<Components::Name>(e);
                SearchResultItem item;
                item.Id = e.id;
                item.Label = name;
                item.Detail = nameComponent && !nameComponent->View().empty() ? Editor::EntityIdLabel(e) : std::string{};
                item.UserData = e;
                results.push_back(std::move(item));
            }
        }
        sink(std::move(results), true);
    }

    void CancelSearch() override {}
    std::string GetPlaceholderText() const override { return "Search entities..."; }

private:
    ECS::World* m_World;
    std::vector<ECS::ComponentTypeId> m_Required;
};
} // namespace

EntityField::EntityField()
{
    AddClass("entity-field");
    BuildNameAndClearButton();
}

void EntityField::SetWorld(ECS::World* world)
{
    m_World = world;
    UpdateDisplay();
}

void EntityField::SetRequiredComponents(std::vector<ECS::ComponentTypeId> required)
{
    m_Required = std::move(required);
}

void EntityField::SetValue(ECS::EntityHandle handle)
{
    if (m_Value == handle)
        return;
    m_Value = handle;
    UpdateDisplay();
}

void EntityField::Clear()
{
    ApplyValue(ECS::EntityHandle());
}

void EntityField::ApplyValue(ECS::EntityHandle handle)
{
    if (m_Value == handle)
        return;
    m_Value = handle;
    UpdateDisplay();
    if (m_OnValueChanged)
        m_OnValueChanged(m_Value);
}

void EntityField::UpdateDisplay()
{
    const bool valid = m_Value.IsValid() && m_World && m_World->IsValid(m_Value);
    if (!valid)
        ShowEmpty();
    else
        ShowNamed(Editor::EntityDisplayName(*m_World, m_Value));
}

std::unique_ptr<ISearchProvider> EntityField::CreateProvider()
{
    if (!m_World)
        return nullptr;
    return std::make_unique<EntitySearchProvider>(m_World, m_Required);
}

void EntityField::OnResultSelected(const SearchResultItem& item)
{
    if (const auto* h = std::any_cast<ECS::EntityHandle>(&item.UserData))
        ApplyValue(*h);
}

SearchItemId EntityField::CurrentResultId() const
{
    return static_cast<SearchItemId>(m_Value.id);
}

void EntityField::PositionDialog(SearchDialog& dialog)
{
    // Match wide fields, but retain a useful search width in a narrow Inspector.
    // Anchoring the right edges prevents that minimum-width popup from spilling
    // past the field on the Inspector's window edge.
    constexpr float kMinEntitySearchWidthPx = 360.0f;
    dialog.SetPanelWidth(std::max(kMinEntitySearchWidthPx, GetLayoutWidth()));
    dialog.SetAnchorPosition(GetLayoutX() + GetLayoutWidth(), GetLayoutY() + GetLayoutHeight(),
                             SearchDialogHorizontalAnchor::TrailingRight, GetLayoutHeight());
}


bool EntityField::MeetsRequirements(ECS::EntityHandle handle) const
{
    if (!m_World || !handle.IsValid() || !m_World->IsValid(handle))
        return false;
    return EntityHasAll(m_World, handle, m_Required);
}

// -- IDropTarget --------------------------------------------------------------

bool EntityField::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::HierarchyEntityDragPayload>();
}

UI::Interaction::DropFeedback EntityField::CanDrop(const UI::Interaction::DropRequest& request) const
{
    const ECS::EntityHandle handle = Editor::DecodeFirstHierarchyEntity(request);
    if (!handle.IsValid() || !m_World || !m_World->IsValid(handle))
        return {false, "Invalid entity"};
    // MeetsRequirements re-checks validity (harmless here) plus the required set.
    if (!MeetsRequirements(handle))
        return {false, "Entity missing required component"};
    return {true, {}};
}

void EntityField::PerformDrop(const UI::Interaction::DropRequest& request)
{
    // MeetsRequirements covers validity + the required set (true when no requirements).
    const ECS::EntityHandle handle = Editor::DecodeFirstHierarchyEntity(request);
    if (MeetsRequirements(handle))
        ApplyValue(handle);
}

} // namespace GameEngine
