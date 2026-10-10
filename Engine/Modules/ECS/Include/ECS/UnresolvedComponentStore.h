#pragma once

#include "ECS/ECS.h" // EntityHandle + EntityHandleHash

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine::ECS
{

// One scene component captured verbatim at load time because its type was not registered yet
// (typically a user's native C++ component whose module is still compiling when the scene loads).
// Name + Props keep the AUTHORED spelling (trimmed) straight from the .scene file so a save
// echoes the input bytes; consumers that resolve them (schema/registry lookups) match
// case-insensitively. An empty Props list means a bare "add defaults" line.
struct PreservedComponent
{
    std::string Name;                                       // e.g. "Jumpable"
    std::vector<std::pair<std::string, std::string>> Props; // {propNameAuthored, rawValue}
};

// One field of a KNOWN component whose authored text this binary cannot represent — typically an
// enumerator added to the .scene by a newer build than the one loading it. The component itself
// loaded and its other fields are live; only this one assignment could not be applied, so the field
// fell back to a default. Keeping the authored text here is what stops a save from writing that
// default over the user's data.
struct PreservedField
{
    std::string Component; // authored component name, e.g. "SplineFence"
    std::string Field;     // authored property key, e.g. "grade"
    std::string RawText;   // the authored value, verbatim

    // What the FIELD ITSELF held immediately after the failed apply, and where it lives. The
    // save-side substitution stands only while those exact bytes are still there: anything that
    // writes this field changes them, and from then on the live value is the truth.
    //
    // Field-scoped BY CONSTRUCTION, which is what makes it survive the things that defeat coarser
    // signals. Adding a component to the entity relocates the row without changing the preserved
    // component's bytes. A write to a SIBLING field of the same component lands in a different byte
    // range. No chunk-scoped or entity-scoped state is consulted at all, so there is no shared
    // counter an unrelated edit could bump.
    //
    // ACCEPTED RESIDUAL, deliberately left open: a write that sets the field to exactly its
    // fallback bytes is invisible here. Do NOT "close" it with a broader signal — a column version,
    // a per-component dirty flag, anything entity- or chunk-scoped — because every such signal is
    // tripped by unrelated edits and retires live overrides, which is the data loss this table
    // exists to prevent. The residual is benign: substituting over a value identical to the
    // fallback leaves the world exactly where treating that write as a no-op would.
    ComponentTypeId TypeId = 0;
    std::uint32_t FieldOffset = 0;
    std::uint32_t FieldSize = 0; // 0 => no reflected FieldInfo; the string tier below applies
    std::vector<std::uint8_t> FallbackBytes;

    // String tier, for a field whose component has a hand-written schema and so no reflected
    // FieldInfo to give a byte range. Compares what the field SERIALIZES to instead.
    //
    // Captured on first save, from the live save context, rather than at load: a value's serialized
    // form can depend on that context (an asset reference resolves through the save's resolver), so
    // a text sampled at load through a stub context describes a save that never happens and cannot
    // compare equal to the real one.
    std::string FallbackText;
    bool FallbackTextCaptured = false;
};

// Per-World side-table for scene data this binary could not turn into live components. Three kinds,
// deliberately separate because they resolve on different events:
//   - PreservedComponent: the whole TYPE was unknown; resolves when a module registers the type.
//   - PreservedField:     the type is known but one VALUE was unrepresentable; resolves when the
//                         value becomes representable, or is superseded by a real write.
//   - Unresolved reference: an asset reference loaded, but its GUID named no asset and the
//                         component keeps only the GUID. The GUID is kept here with the path the scene
//                         authored, so a save writes the path back and the editor can name it; once
//                         the GUID resolves the save writes the asset's own path instead and the entry
//                         no longer matters.
// This lives OUTSIDE ECS archetype storage: components must be trivially copyable (they are memcpy'd
// between chunks), and this holds std::string/std::vector. The first two kinds are keyed by
// EntityHandle so an entry survives the load -> (later) module-load -> save timeline, after which the
// editor's re-apply pass instantiates the now-registered type and erases the entry. Main-thread
// access only.
class UnresolvedComponentStore
{
  public:
    void Add(EntityHandle entity, PreservedComponent component)
    {
        m_ByEntity[entity].push_back(std::move(component));
    }

    void AddField(EntityHandle entity, PreservedField field)
    {
        m_FieldsByEntity[entity].push_back(std::move(field));
    }

    // Empty() answers only for whole preserved COMPONENTS, because that is what its existing callers
    // (the save re-emit and the editor re-apply pass) iterate. Field overrides have their own probe.
    bool Empty() const { return m_ByEntity.empty(); }
    bool FieldsEmpty() const { return m_FieldsByEntity.empty(); }

    // Drops every entry of every kind. World::Clear uses this; clearing one table and forgetting
    // another would leak a previous scene's overrides onto the next one's entity handles.
    void Clear()
    {
        m_ByEntity.clear();
        m_FieldsByEntity.clear();
        m_ReferencePathsByGuid.clear();
    }

    // Record an asset reference whose GUID named no asset, with the path the scene text authored
    // for it (empty when it authored none). `guid` is the GUID's canonical text (this module has
    // no GUID type). A later note for the same GUID replaces the earlier one, so references that
    // authored different paths for one GUID all save with the last one loaded.
    void NoteUnresolvedReference(std::string guid, std::string authoredPath)
    {
        m_ReferencePathsByGuid[std::move(guid)] = std::move(authoredPath);
    }

    // The authored path recorded for an unresolved `guid` (empty when the scene authored none), or
    // null when the GUID was not recorded as unresolved.
    const std::string* FindUnresolvedReference(const std::string& guid) const
    {
        auto it = m_ReferencePathsByGuid.find(guid);
        return it == m_ReferencePathsByGuid.end() ? nullptr : &it->second;
    }

    // Drop the field overrides belonging to one component of one entity. Removing the component
    // destroys the instance those overrides describe: a later re-add is a NEW component holding
    // defaults, and the previous load's authored text must not reappear on it.
    void EraseFieldsOfComponent(EntityHandle entity, ComponentTypeId typeId)
    {
        auto it = m_FieldsByEntity.find(entity);
        if (it == m_FieldsByEntity.end())
            return;
        std::vector<PreservedField>& fields = it->second;
        std::erase_if(fields, [typeId](const PreservedField& f) { return f.TypeId == typeId; });
        if (fields.empty())
            m_FieldsByEntity.erase(it);
    }

    // Drop one field's override outright, on the user's explicit say-so.
    //
    // The byte comparison cannot see a write that sets the field to exactly its fallback, so a user
    // who ACCEPTS the value the field fell back to cannot express that by editing alone. This is
    // how they express it: the inspector shows the authored text it could not read and offers to
    // discard it, and discarding is what lets the next save write the live value. Matching is
    // case-insensitive on the authored key, which is the spelling the inspector displays.
    bool DiscardField(EntityHandle entity, ComponentTypeId typeId, std::string_view authoredField)
    {
        auto it = m_FieldsByEntity.find(entity);
        if (it == m_FieldsByEntity.end())
            return false;
        const auto equalsFold = [](std::string_view a, std::string_view b)
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
        };
        const std::size_t before = it->second.size();
        std::erase_if(it->second,
                      [&](const PreservedField& f)
                      { return f.TypeId == typeId && equalsFold(f.Field, authoredField); });
        const bool erased = it->second.size() != before;
        if (it->second.empty())
            m_FieldsByEntity.erase(it);
        return erased;
    }

    // Drop every override belonging to an entity that no longer exists. The overrides describe
    // component instances the destroy just took with it.
    //
    // Only for a destroy that RELEASES the handle. The preserve-handle destroy is editor undo/redo
    // parking the same EntityHandle for revival, and dropping the overrides there would lose the
    // authored text across a delete-then-undo — the very loss this table prevents.
    void EraseEntity(EntityHandle entity)
    {
        m_ByEntity.erase(entity);
        m_FieldsByEntity.erase(entity);
    }

    // Mutable access to one entity's field overrides, for the save-side string-tier capture that
    // can only be taken once a real save context exists.
    std::vector<PreservedField>* FieldsForMutable(EntityHandle entity)
    {
        auto it = m_FieldsByEntity.find(entity);
        return it == m_FieldsByEntity.end() ? nullptr : &it->second;
    }

    // Overwrite a preserved property's raw value in place (editor edit of an unloaded
    // component). propName is matched verbatim against the stored, parsed prop name.
    // Returns true when the entity/component/prop triple was found and updated. No-op
    // (returns false) otherwise — the store never invents entries from a setter.
    bool SetProp(EntityHandle entity, const std::string& componentName,
                 const std::string& propName, std::string value)
    {
        auto it = m_ByEntity.find(entity);
        if (it == m_ByEntity.end())
            return false;
        for (PreservedComponent& comp : it->second)
        {
            if (comp.Name != componentName)
                continue;
            for (auto& prop : comp.Props)
            {
                if (prop.first == propName)
                {
                    prop.second = std::move(value);
                    return true;
                }
            }
        }
        return false;
    }

    using Table = std::unordered_map<EntityHandle, std::vector<PreservedComponent>, EntityHandleHash>;
    using FieldTable = std::unordered_map<EntityHandle, std::vector<PreservedField>, EntityHandleHash>;

    Table& Map() { return m_ByEntity; }
    const Table& Map() const { return m_ByEntity; }

    // Null when this entity has no field overrides — the common case, so callers can bail before
    // touching any string.
    const std::vector<PreservedField>* FieldsFor(EntityHandle entity) const
    {
        auto it = m_FieldsByEntity.find(entity);
        return it == m_FieldsByEntity.end() ? nullptr : &it->second;
    }

    FieldTable& FieldMap() { return m_FieldsByEntity; }
    const FieldTable& FieldMap() const { return m_FieldsByEntity; }

  private:
    Table m_ByEntity;
    FieldTable m_FieldsByEntity;
    std::unordered_map<std::string, std::string> m_ReferencePathsByGuid;
};

// The bytes `field` names (FieldOffset/FieldSize of TypeId) as they stand on `entity` right now,
// into `out`. False when the field has no byte range, or the entity lacks the component or its
// bytes are shorter than the range. Reads through World::CaptureComponentBytes, which is const and
// never stamps: asking what a field holds must not look like writing it.
[[nodiscard]] bool CapturePreservedFieldBytes(const World& world, EntityHandle entity,
                                              const PreservedField& field, std::vector<std::uint8_t>& out);

// Whether a real write has superseded `field`'s preserved text: its live bytes no longer equal
// FallbackBytes, or can no longer be read. A string-tier field (FieldSize 0) is decided only at
// save, from its serialized text, so this reports false for it.
[[nodiscard]] bool IsPreservedFieldSuperseded(const World& world, EntityHandle entity,
                                              const PreservedField& field);

} // namespace GameEngine::ECS
