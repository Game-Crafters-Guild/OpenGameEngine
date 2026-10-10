#include "PageReflection.h"

#include "Core/EngineVersion.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"
#include "Types/FlatMap.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <utility>

namespace GameEngine::WebLibrary
{

namespace
{

bool IsPageField(const ECS::FieldInfo& field)
{
    return field.Type != ECS::FieldTypeId::Unknown &&
           !ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::Hidden | ECS::FieldFlags::Transient);
}

std::string_view UnqualifiedName(std::string_view canonicalName)
{
    const size_t separator = canonicalName.rfind("::");
    return separator == std::string_view::npos ? canonicalName : canonicalName.substr(separator + 2);
}

// The member name as the generated components.d.ts spells it: the leading capitals lowered,
// all but the last of a run that a lower-case letter follows (`Intensity` -> `intensity`,
// `RGBMode` -> `rgbMode`). The component scanner's CamelCase, letter for letter.
std::string TsName(std::string_view name)
{
    size_t run = 0;
    while (run < name.size() && name[run] >= 'A' && name[run] <= 'Z')
        ++run;
    std::string result(name);
    const size_t lower = (run == name.size() || run <= 1) ? run : run - 1;
    for (size_t i = 0; i < lower; ++i)
        result[i] = static_cast<char>(result[i] - 'A' + 'a');
    return result;
}

nlohmann::ordered_json FieldJson(const ECS::FieldInfo& field)
{
    nlohmann::ordered_json json;
    json["name"] = std::string(field.Name);
    json["tsName"] = TsName(field.Name);
    json["kind"] = std::string(KindName(field.Type));
    json["offset"] = field.Offset;
    json["size"] = field.Size;
    json["count"] = field.Size / ElementSize(field.Type);
    if (!field.EnumNames.empty())
    {
        nlohmann::ordered_json enumerators = nlohmann::ordered_json::array();
        for (const ECS::EnumNameValue& enumerator : field.EnumNames)
            enumerators.push_back({{"name", std::string(enumerator.Name)}, {"value", enumerator.Value}});
        json["enum"] = std::move(enumerators);
    }
    if (ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::ReadOnly))
        json["readOnly"] = true;
    return json;
}

bool BuildPageComponent(std::uint64_t typeId, PageComponent& outComponent)
{
    if (!ECS::ComponentFieldRegistry::Has(typeId) || ECS::ComponentFieldRegistry::IsComponentEditorOnly(typeId))
        return false;
    const std::string_view canonicalName = ECS::ComponentFieldRegistry::GetCanonicalName(typeId);
    if (canonicalName.empty())
        return false;
    outComponent.TypeId = typeId;
    outComponent.Name = UnqualifiedName(canonicalName);
    outComponent.Fields.clear();
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
    {
        if (IsPageField(field))
            outComponent.Fields.push_back(&field);
    }
    return true;
}

// A page component as first built, with the registry's field table it was built from: the table's
// address changes when the registry copies or replaces it, which is when the entry is rebuilt.
struct CachedPageComponent
{
    PageComponent Component;
    const ECS::FieldInfo* SourceFields = nullptr;
    std::size_t SourceFieldCount = 0;
};

FlatMap<std::uint64_t, CachedPageComponent>& PageComponentCache()
{
    static FlatMap<std::uint64_t, CachedPageComponent> cache;
    return cache;
}

} // namespace

const PageComponent* FindPageComponent(std::uint64_t typeId)
{
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    FlatMap<std::uint64_t, CachedPageComponent>& cache = PageComponentCache();
    if (const CachedPageComponent* cached = cache.Find(typeId);
        cached && cached->SourceFields == fields.data() && cached->SourceFieldCount == fields.size())
    {
        return &cached->Component;
    }
    CachedPageComponent entry;
    if (!BuildPageComponent(typeId, entry.Component))
    {
        cache.Erase(typeId);
        return nullptr;
    }
    entry.SourceFields = fields.data();
    entry.SourceFieldCount = fields.size();
    return &cache.InsertOrAssign(typeId, std::move(entry)).Component;
}

std::vector<PageComponent> ListPageComponents()
{
    // Every reflected component records its default bytes when it registers; a hand-written
    // registration may record none but has a handler. A handler alone is not enough: the ECS
    // registers one on a component's first use, so it misses components no entity has held.
    std::vector<std::uint64_t> typeIds = ECS::ComponentFactory::DefaultByteTypes();
    for (const auto& [typeId, handler] : ECS::ComponentRegistry::GetAllHandlers())
        typeIds.push_back(typeId);
    std::sort(typeIds.begin(), typeIds.end());
    typeIds.erase(std::unique(typeIds.begin(), typeIds.end()), typeIds.end());

    std::vector<PageComponent> components;
    for (const std::uint64_t typeId : typeIds)
    {
        PageComponent component;
        if (BuildPageComponent(typeId, component))
            components.push_back(std::move(component));
    }
    std::sort(components.begin(), components.end(),
              [](const PageComponent& a, const PageComponent& b) { return a.Name < b.Name; });
    return components;
}

std::uint32_t ElementSize(ECS::FieldTypeId type)
{
    using ECS::FieldTypeId;
    switch (type)
    {
    case FieldTypeId::Int16:
    case FieldTypeId::UInt16:
        return 2;
    case FieldTypeId::Int32:
    case FieldTypeId::UInt32:
    case FieldTypeId::Float:
    case FieldTypeId::EntityHandle:
        return 4;
    case FieldTypeId::Int64:
    case FieldTypeId::UInt64:
    case FieldTypeId::Double:
    case FieldTypeId::Vec2:
        return 8;
    case FieldTypeId::Vec3:
        return 12;
    case FieldTypeId::Vec4:
    case FieldTypeId::Quat:
    case FieldTypeId::Color:
    case FieldTypeId::AssetGuid:
        return 16;
    case FieldTypeId::Mat4:
        return 64;
    default:
        return 1;
    }
}

std::string_view KindName(ECS::FieldTypeId type)
{
    using ECS::FieldTypeId;
    switch (type)
    {
    case FieldTypeId::Bool: return "Bool";
    case FieldTypeId::Int8: return "Int8";
    case FieldTypeId::Int16: return "Int16";
    case FieldTypeId::Int32: return "Int32";
    case FieldTypeId::Int64: return "Int64";
    case FieldTypeId::UInt8: return "UInt8";
    case FieldTypeId::UInt16: return "UInt16";
    case FieldTypeId::UInt32: return "UInt32";
    case FieldTypeId::UInt64: return "UInt64";
    case FieldTypeId::Float: return "Float";
    case FieldTypeId::Double: return "Double";
    case FieldTypeId::Vec2: return "Vec2";
    case FieldTypeId::Vec3: return "Vec3";
    case FieldTypeId::Vec4: return "Vec4";
    case FieldTypeId::Quat: return "Quat";
    case FieldTypeId::Mat4: return "Mat4";
    case FieldTypeId::Color: return "Color";
    case FieldTypeId::AssetGuid: return "AssetGuid";
    case FieldTypeId::EntityHandle: return "EntityHandle";
    case FieldTypeId::String: return "String";
    case FieldTypeId::Bytes: return "Bytes";
    case FieldTypeId::Unknown: break;
    }
    return "Unknown";
}

std::string BuildReflectionJson()
{
    nlohmann::ordered_json components = nlohmann::ordered_json::array();
    for (const PageComponent& component : ListPageComponents())
    {
        nlohmann::ordered_json fields = nlohmann::ordered_json::array();
        for (const ECS::FieldInfo* field : component.Fields)
            fields.push_back(FieldJson(*field));
        nlohmann::ordered_json json;
        json["name"] = std::string(component.Name);
        // Decimal text: a JSON number cannot carry 64 bits.
        json["typeId"] = std::to_string(component.TypeId);
        json["fields"] = std::move(fields);
        components.push_back(std::move(json));
    }
    nlohmann::ordered_json document;
    document["engineVersion"] = kEngineVersion;
    document["components"] = std::move(components);
    return document.dump();
}

} // namespace GameEngine::WebLibrary
