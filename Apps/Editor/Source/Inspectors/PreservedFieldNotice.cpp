#include "Inspectors/PreservedFieldNotice.h"

#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/UIElement.h"

#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

bool EqualsFold(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    const auto lower = [](char c)
    { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

const ECS::PreservedField* FindPreservedField(const ECS::World& world, ECS::EntityHandle entity,
                                              ECS::ComponentTypeId typeId, std::string_view fieldName)
{
    const ECS::UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    if (!store)
        return nullptr;
    const std::vector<ECS::PreservedField>* fields = store->FieldsFor(entity);
    if (!fields)
        return nullptr;
    for (const ECS::PreservedField& f : *fields)
    {
        if (f.TypeId == typeId && EqualsFold(f.Field, fieldName))
            return &f;
    }
    return nullptr;
}

// Keyed on the handle rather than the PreservedField pointer: discarding rewrites the store's
// vector, so any pointer into it dies with the erase.
void DiscardPreservedField(ECS::World& world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                           const std::string& field, const std::function<void()>& onDiscarded)
{
    if (!world.IsValid(entity))
        return;
    world.GetUnresolvedComponents().DiscardField(entity, typeId, field);
    if (onDiscarded)
        onDiscarded();
}

// The notice itself: what the user authored, that it could not be read, and the one action that
// resolves the disagreement toward the live value.
//
// `nameTheField` distinguishes the two placements. Inline above the row it describes, the field is
// already named by that row and repeating it is noise. Hoisted to the top of a component's section,
// nothing else says which field is meant, so the text carries the name.
void EmitNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity,
                ECS::ComponentTypeId typeId, const ECS::PreservedField& pf, bool nameTheField,
                const std::function<void()>& onDiscarded)
{
    // The authored text first: it is the thing the user wrote and cannot see anywhere else.
    auto notice = std::make_unique<EditorUI::InspectorNotice>(
        nameTheField ? "Authored value \"" + pf.RawText + "\" for " + pf.Field +
                           " could not be read by this build. That field shows what it fell back "
                           "to; saving writes the authored value back, not that fallback."
                     : "Authored value \"" + pf.RawText +
                           "\" could not be read by this build. The field below shows what it fell "
                           "back to; saving writes the authored value back, not that fallback.");

    ECS::World* worldPtr = &world;
    const std::string authoredField = pf.Field;
    notice->SetAction("Discard preserved value",
                      "Forget the authored value and let this field's current value be saved",
                      [worldPtr, entity, typeId, authoredField, onDiscarded]()
                      { DiscardPreservedField(*worldPtr, entity, typeId, authoredField, onDiscarded); });
    parent->AddChild(std::move(notice));
}

} // namespace

void AddPreservedFieldNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity,
                             ECS::ComponentTypeId typeId, std::string_view fieldName,
                             std::function<void()> onDiscarded)
{
    if (!parent)
        return;
    const ECS::PreservedField* pf = FindPreservedField(world, entity, typeId, fieldName);
    if (!pf)
        return;

    EmitNotice(parent, world, entity, typeId, *pf, /*nameTheField=*/false, onDiscarded);
}

void AddPreservedComponentFieldsNotice(UIElement* parent, ECS::World& world,
                                       ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                                       std::function<void()> onDiscarded)
{
    if (!parent)
        return;
    const ECS::UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    if (!store)
        return;
    const std::vector<ECS::PreservedField>* fields = store->FieldsFor(entity);
    if (!fields)
        return;

    // Copied, not iterated in place: each notice's discard rewrites this very vector, and a click
    // during the loop would invalidate it. The copy is of the fields of one component of one
    // entity — the degraded case, and small by construction.
    std::vector<ECS::PreservedField> ofThisComponent;
    for (const ECS::PreservedField& f : *fields)
    {
        if (f.TypeId == typeId)
            ofThisComponent.push_back(f);
    }

    for (const ECS::PreservedField& pf : ofThisComponent)
        EmitNotice(parent, world, entity, typeId, pf, /*nameTheField=*/true, onDiscarded);
}

} // namespace GameEngine::Editor
