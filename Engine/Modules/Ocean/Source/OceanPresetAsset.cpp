#include "Ocean/OceanPresetAsset.h"

#include "Components/Rendering/Ocean.h"
#include "AssetCore/SharedFileRead.h"
#include "AssetCore/GUID.h"
#include "ECS/ECSTemplates.h"
#include <cctype>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace GameEngine::Ocean
{
namespace
{
void SetOceanPresetError(std::string* error, const char* text)
{
    if (error)
        *error = text;
}

// The identities of the fields a binding keeps, without its unused (zero) entries,
// so each field compares against the short list.
std::span<const StringId> CollectOverriddenFields(
    std::span<const Components::OceanPresetOverride> overrides,
    std::span<StringId, Components::kOceanPresetOverrideCapacity> storage)
{
    size_t count = 0u;
    for (const Components::OceanPresetOverride& entry :
         overrides.first(std::min(overrides.size(), storage.size())))
    {
        if (entry.FieldIdentifier != 0u)
            storage[count++] = entry.FieldIdentifier;
    }
    return storage.first(count);
}

template <typename T>
bool ReadScalar(const OceanPresetValue& value, T& out)
{
    return std::visit([&](const auto& stored) -> bool {
        using U = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<U, bool> || std::is_integral_v<U> ||
                      std::is_floating_point_v<U>)
        {
            out = static_cast<T>(stored);
            return true;
        }
        else
        {
            return false;
        }
    }, value);
}

size_t ElementSize(ECS::FieldTypeId type)
{
    using ECS::FieldTypeId;
    switch (type)
    {
    case FieldTypeId::Bool: return sizeof(bool);
    case FieldTypeId::Int8: case FieldTypeId::UInt8: return 1u;
    case FieldTypeId::Int16: case FieldTypeId::UInt16: return 2u;
    case FieldTypeId::Int32: case FieldTypeId::UInt32: case FieldTypeId::Float: return 4u;
    case FieldTypeId::Int64: case FieldTypeId::UInt64: case FieldTypeId::Double: return 8u;
    case FieldTypeId::Color: return sizeof(float32);
    default: return 0u;
    }
}

template <typename T>
bool WriteNumbers(const OceanPresetValue& value, std::byte* destination, size_t count)
{
    if (const auto* array = std::get_if<OceanPresetNumberArray>(&value))
    {
        if (array->size() < count)
            return false;
        for (size_t i = 0u; i < count; ++i)
        {
            const T converted = static_cast<T>((*array)[i]);
            std::memcpy(destination + i * sizeof(T), &converted, sizeof(T));
        }
        return true;
    }
    if (count != 1u)
        return false;
    T scalar{};
    if (!ReadScalar(value, scalar))
        return false;
    std::memcpy(destination, &scalar, sizeof(T));
    return true;
}

bool WriteFieldValue(const ECS::FieldInfo& field, const OceanPresetValue& value,
                     std::byte* component, size_t componentBytes)
{
    if (static_cast<size_t>(field.Offset) + field.Size > componentBytes)
        return false;
    std::byte* destination = component + field.Offset;
    const size_t elementSize = ElementSize(field.Type);
    const size_t count = elementSize > 0u ? field.Size / elementSize : 0u;
    using ECS::FieldTypeId;
    switch (field.Type)
    {
    case FieldTypeId::Bool: return WriteNumbers<bool>(value, destination, count);
    case FieldTypeId::Int8: return WriteNumbers<int8>(value, destination, count);
    case FieldTypeId::Int16: return WriteNumbers<int16>(value, destination, count);
    case FieldTypeId::Int32: return WriteNumbers<int32>(value, destination, count);
    case FieldTypeId::Int64: return WriteNumbers<int64>(value, destination, count);
    case FieldTypeId::UInt8: return WriteNumbers<uint8>(value, destination, count);
    case FieldTypeId::UInt16: return WriteNumbers<uint16>(value, destination, count);
    case FieldTypeId::UInt32: return WriteNumbers<uint32>(value, destination, count);
    case FieldTypeId::UInt64: return WriteNumbers<uint64>(value, destination, count);
    case FieldTypeId::Float: return WriteNumbers<float32>(value, destination, count);
    case FieldTypeId::Double: return WriteNumbers<double>(value, destination, count);
    case FieldTypeId::Color: return WriteNumbers<float32>(value, destination, field.Size / sizeof(float32));
    case FieldTypeId::AssetGuid:
        if (const auto* text = std::get_if<std::string>(&value); text && field.Size == sizeof(GUID))
        {
            std::string digits = *text;
            digits.erase(std::remove(digits.begin(), digits.end(), '-'), digits.end());
            if (!digits.empty() && (digits.size() != 32 || !std::all_of(digits.begin(), digits.end(),
                [](unsigned char c) { return std::isxdigit(c) != 0; }))) return false;
            const GUID guid = digits.empty() ? GUID::Null() : GUID(digits);
            std::memcpy(destination, guid.GetData().data(), sizeof(GUID));
            return true;
        }
        return false;
    case FieldTypeId::String:
        if (const auto* stringValue = std::get_if<std::string>(&value))
        {
            const size_t copy = std::min(stringValue->size(), static_cast<size_t>(field.Size - 1u));
            std::memset(destination, 0, field.Size);
            std::memcpy(destination, stringValue->data(), copy);
            return true;
        }
        return false;
    default: return false;
    }
}

nlohmann::json ToJson(const OceanPresetValue& value)
{
    return std::visit([](const auto& stored) -> nlohmann::json { return stored; }, value);
}

OceanPresetValue FromJson(const nlohmann::json& value)
{
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_unsigned()) return value.get<uint64>();
    if (value.is_number_integer()) return value.get<int64>();
    if (value.is_number_float()) return value.get<double>();
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array()) return value.get<OceanPresetNumberArray>();
    return std::string{};
}
} // namespace

void OceanPresetAsset::Set(std::string fieldPath, OceanPresetValue value)
{
    const StringId fieldIdentifier = HashStringId(fieldPath);
    m_Values[std::move(fieldPath)] = Entry{std::move(value), fieldIdentifier};
    ++Revision;
}

bool OceanPresetAsset::Remove(std::string_view fieldPath)
{
    const size_t removed = m_Values.erase(std::string(fieldPath));
    if (removed)
        ++Revision;
    return removed != 0u;
}

const OceanPresetValue* OceanPresetAsset::Find(std::string_view fieldPath) const
{
    const auto found = m_Values.find(std::string(fieldPath));
    return found == m_Values.end() ? nullptr : &found->second.Value;
}

bool OceanPresetAsset::SaveJson(const std::filesystem::path& path, std::string* error) const
{
    nlohmann::json values = nlohmann::json::object();
    for (const auto& [key, entry] : m_Values)
        values[key] = ToJson(entry.Value);
    nlohmann::json document{{"FormatVersion", kFormatVersion},
                            {"Revision", Revision}, {"Values", std::move(values)}};
    std::ofstream stream(path, std::ios::trunc);
    if (!stream)
    {
        SetOceanPresetError(error, "failed to open ocean preset for writing");
        return false;
    }
    stream << document.dump(2) << '\n';
    return static_cast<bool>(stream);
}

bool OceanPresetAsset::LoadJson(const std::filesystem::path& path, std::string* error)
{
    String text;
    if (!ReadFileTextShared(path, text))
    {
        SetOceanPresetError(error, "failed to open ocean preset");
        return false;
    }
    nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded())
    {
        SetOceanPresetError(error, "invalid ocean preset JSON");
        return false;
    }
    if (document.value("FormatVersion", 0u) != kFormatVersion ||
        !document.contains("Values") || !document["Values"].is_object())
    {
        SetOceanPresetError(error, "unsupported or malformed ocean preset");
        return false;
    }
    std::unordered_map<std::string, Entry> loaded;
    for (auto it = document["Values"].begin(); it != document["Values"].end(); ++it)
    {
        if (it.value().is_object() || it.value().is_null())
            continue;
        loaded[it.key()] = Entry{FromJson(it.value()), HashStringId(it.key())};
    }
    m_Values = std::move(loaded);
    Revision = document.value("Revision", Revision + 1u);
    return true;
}

uint32 OceanPresetAsset::Apply(std::string_view prefix, ECS::ComponentTypeId typeId,
                               void* component, size_t componentBytes,
                               std::span<const Components::OceanPresetOverride> overrides) const
{
    if (!component)
        return 0u;
    std::array<StringId, Components::kOceanPresetOverrideCapacity> overriddenStorage;
    const std::span<const StringId> overridden =
        CollectOverriddenFields(overrides, overriddenStorage);
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    uint32 applied = 0u;
    // One path buffer for every field: it stops allocating once it holds the longest.
    std::string fieldPath(prefix);
    fieldPath += '.';
    const size_t rootLength = fieldPath.size();
    for (const ECS::FieldInfo& field : fields)
    {
        fieldPath.resize(rootLength);
        fieldPath += field.Name;
        const auto found = m_Values.find(fieldPath);
        if (found == m_Values.end() ||
            std::ranges::find(overridden, found->second.FieldIdentifier) != overridden.end())
        {
            continue;
        }
        if (WriteFieldValue(field, found->second.Value,
                            static_cast<std::byte*>(component), componentBytes))
        {
            ++applied;
        }
    }
    return applied;
}

std::span<const Components::OceanPresetOverride> PresetOverridesOf(const ECS::World& world,
                                                                   ECS::EntityHandle entity)
{
    const auto* binding = world.GetComponent<Components::OceanPresetBinding>(entity);
    if (!binding)
        return {};
    return binding->Overrides;
}

void OceanPresetInstance::SetOverride(std::string fieldPath, OceanPresetValue value)
{
    m_Overrides[std::move(fieldPath)] = std::move(value);
}

bool OceanPresetInstance::ResetField(std::string_view fieldPath)
{
    return m_Overrides.erase(std::string(fieldPath)) != 0u;
}

void OceanPresetInstance::ResetAll()
{
    m_Overrides.clear();
}

bool OceanPresetInstance::IsOverridden(std::string_view fieldPath) const
{
    return m_Overrides.find(std::string(fieldPath)) != m_Overrides.end();
}

const OceanPresetValue* OceanPresetInstance::Resolve(std::string_view fieldPath) const
{
    const auto overrideValue = m_Overrides.find(std::string(fieldPath));
    if (overrideValue != m_Overrides.end())
        return &overrideValue->second;
    return m_Base ? m_Base->Find(fieldPath) : nullptr;
}

} // namespace GameEngine::Ocean
