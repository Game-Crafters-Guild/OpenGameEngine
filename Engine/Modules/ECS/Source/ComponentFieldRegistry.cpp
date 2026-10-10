#include "ECS/ComponentFieldRegistry.h"

#include "ECS/ModuleRegistration.h"

#include <cctype>
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace ECS {

namespace {
struct Entry
{
    // Non-owning view of the type's constexpr Reflection<T>::Fields by default (zero per-component
    // heap). The first per-field mutation (SetFieldFlags / SetFieldEnum) copies the table into Owned
    // ONCE (copy-on-write) and re-points Fields at it — the constexpr source is never touched, so a
    // component with no policy flags / enum bindings stays copy-free.
    std::span<const FieldInfo> Fields;
    std::vector<FieldInfo> Owned;  // filled lazily on first mutation; Fields then views it
    std::string_view CanonicalName;
    // RegisterOwned storage: field names + canonical name deep-copied from the caller (the
    // scripting ABI passes marshaled temporaries). OwnedStrings is filled once per registration
    // and never resized afterwards — Owned's Name views point into it, and a resize would move
    // the (SSO) string objects out from under them.
    std::vector<std::string> OwnedStrings;
    std::string OwnedName;
    bool DoNotSerialize = false;   // "// [DoNotSerialize]" runtime/derived component — scene skips it
    bool EditorOnly = false;       // "// @ge-editor-only" — a game export strips it from staged scenes
    // Owning module + load generation (C12). The default Register path keeps
    // NON-OWNING views into the registrant's constexpr tables (module rdata),
    // so entries of an unmapping module MUST be purged with it, and an entry
    // still stamped with an older generation blocks that image's unload.
    ModuleRegistrationStamp Module;
};

// Node-based map: an Entry (and its Owned storage) is address-stable across other inserts, so a span
// returned by Get() stays valid.
std::unordered_map<ComponentTypeId, Entry>& FieldTables()
{
    static std::unordered_map<ComponentTypeId, Entry> tables;
    return tables;
}

// Copy the constexpr field view into Owned the first time a field is mutated, then point Fields at
// Owned. Idempotent: later mutations edit Owned in place (no realloc), so Fields stays valid.
void EnsureOwned(Entry& e)
{
    if (e.Owned.empty() && !e.Fields.empty())
    {
        e.Owned.assign(e.Fields.begin(), e.Fields.end());
        e.Fields = e.Owned;
    }
}
} // namespace

void ComponentFieldRegistry::Register(ComponentTypeId typeId, std::span<const FieldInfo> fields,
                                      std::string_view canonicalName)
{
    Entry& e = FieldTables()[typeId];
    e.Owned.clear();   // re-registration (hot-reload) re-points to the fresh constexpr source;
    e.OwnedStrings.clear();
    if (!e.OwnedName.empty())
        e.CanonicalName = {}; // was viewing OwnedName — don't leave it dangling
    e.OwnedName.clear();
    e.Fields = fields; // any following Set* calls re-apply via copy-on-write
    if (!canonicalName.empty())
        e.CanonicalName = canonicalName;
    e.Module = GetActiveRegistrationModule();
}

void ComponentFieldRegistry::RegisterOwned(ComponentTypeId typeId, std::span<const FieldInfo> fields,
                                           std::string_view canonicalName)
{
    Entry& e = FieldTables()[typeId];
    e.Owned.clear();
    e.OwnedStrings.clear();
    e.OwnedName.clear();

    // Copy the name strings first, sized exactly: the FieldInfo Name views built below point into
    // these strings, so the vector must never reallocate after this loop.
    e.OwnedStrings.reserve(fields.size());
    for (const FieldInfo& f : fields)
        e.OwnedStrings.emplace_back(f.Name);

    e.Owned.assign(fields.begin(), fields.end());
    for (std::size_t i = 0; i < e.Owned.size(); ++i)
        e.Owned[i].Name = e.OwnedStrings[i];
    e.Fields = e.Owned;

    if (!canonicalName.empty())
    {
        e.OwnedName.assign(canonicalName);
        e.CanonicalName = e.OwnedName;
    }
    e.Module = GetActiveRegistrationModule();
}

void ComponentFieldRegistry::UnregisterForTests(ComponentTypeId typeId)
{
    FieldTables().erase(typeId);
}

std::size_t ComponentFieldRegistry::PurgeModuleFieldTables(std::string_view moduleId, std::uint64_t generation)
{
    if (moduleId.empty())
        return 0;
    auto& tables = FieldTables();
    std::size_t purged = 0;
    for (auto it = tables.begin(); it != tables.end();)
    {
        if (it->second.Module.Matches(moduleId, generation))
        {
            it = tables.erase(it);
            ++purged;
        }
        else
        {
            ++it;
        }
    }
    return purged;
}

std::size_t ComponentFieldRegistry::CountSupersededModuleFieldTables(std::string_view moduleId,
                                                                     std::uint64_t currentGeneration)
{
    if (moduleId.empty())
        return 0;
    std::size_t stale = 0;
    for (const auto& [typeId, entry] : FieldTables())
    {
        if (entry.Module.ModuleId == moduleId && entry.Module.Generation < currentGeneration)
            ++stale;
    }
    return stale;
}

std::span<const FieldInfo> ComponentFieldRegistry::Get(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
    {
        return {};
    }
    return it->second.Fields;
}

void ComponentFieldRegistry::SetFieldFlags(ComponentTypeId typeId, std::string_view fieldName,
                                           FieldFlags flags)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    EnsureOwned(it->second);
    for (FieldInfo& f : it->second.Owned)
    {
        if (f.Name == fieldName)
        {
            f.Flags = flags;
            return;
        }
    }
}

void ComponentFieldRegistry::SetFieldRange(ComponentTypeId typeId, std::string_view fieldName,
                                           float minValue, float maxValue)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    EnsureOwned(it->second);
    for (FieldInfo& f : it->second.Owned)
    {
        if (f.Name == fieldName)
        {
            f.MinValue = minValue;
            f.MaxValue = maxValue;
            f.HasRange = true;
            return;
        }
    }
}

void ComponentFieldRegistry::SetFieldTooltip(ComponentTypeId typeId, std::string_view fieldName,
                                             std::string_view tooltip)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    EnsureOwned(it->second);
    for (FieldInfo& f : it->second.Owned)
    {
        if (f.Name == fieldName)
        {
            f.Tooltip = tooltip;
            return;
        }
    }
}

void ComponentFieldRegistry::SetFieldEnum(ComponentTypeId typeId, std::string_view fieldName,
                                          std::span<const EnumNameValue> enumNames)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
        return;
    EnsureOwned(it->second);
    for (FieldInfo& f : it->second.Owned)
    {
        if (f.Name == fieldName)
        {
            f.EnumNames = enumNames;
            return;
        }
    }
}

std::string_view ComponentFieldRegistry::GetCanonicalName(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it == FieldTables().end())
    {
        return {};
    }
    return it->second.CanonicalName;
}

namespace {
bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}
} // namespace

ComponentTypeId ComponentFieldRegistry::FindByName(std::string_view name)
{
    // Case-insensitive: callers include the scene loader (component names are lowercased by the
    // .scene parser) and the editor/MCP (user-typed names), so matching is forgiving on case.
    for (const auto& [typeId, entry] : FieldTables())
    {
        const std::string_view canon = entry.CanonicalName;
        if (canon.empty())
            continue;
        if (IEquals(canon, name))
            return typeId;
        // Simple-name suffix: canon ends with "::name" (native C++) or ".name" (managed blob
        // components register their dotted CLR name, e.g. "Game.Health" matches "Health").
        if (canon.size() >= name.size() + 2 &&
            IEquals(canon.substr(canon.size() - name.size()), name) &&
            canon.substr(canon.size() - name.size() - 2, 2) == "::")
            return typeId;
        if (canon.size() >= name.size() + 1 &&
            IEquals(canon.substr(canon.size() - name.size()), name) &&
            canon[canon.size() - name.size() - 1] == '.')
            return typeId;
    }
    return 0;
}

const FieldInfo* ComponentFieldRegistry::FindField(ComponentTypeId typeId, std::string_view fieldName)
{
    for (const FieldInfo& field : Get(typeId))
    {
        if (IEquals(field.Name, fieldName))
            return &field;
    }
    return nullptr;
}

bool ComponentFieldRegistry::Has(ComponentTypeId typeId)
{
    return FieldTables().contains(typeId);
}

void ComponentFieldRegistry::SetComponentDoNotSerialize(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it != FieldTables().end())
        it->second.DoNotSerialize = true;
}

bool ComponentFieldRegistry::IsComponentDoNotSerialize(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    return it != FieldTables().end() && it->second.DoNotSerialize;
}

void ComponentFieldRegistry::SetComponentEditorOnly(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    if (it != FieldTables().end())
        it->second.EditorOnly = true;
}

bool ComponentFieldRegistry::IsComponentEditorOnly(ComponentTypeId typeId)
{
    const auto it = FieldTables().find(typeId);
    return it != FieldTables().end() && it->second.EditorOnly;
}

} // namespace ECS
} // namespace GameEngine
