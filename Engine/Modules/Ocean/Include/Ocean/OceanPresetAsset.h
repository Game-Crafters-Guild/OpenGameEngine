#pragma once

#include "ECS/ComponentFieldRegistry.h"
#include "Types/Types.h"
#include "Types/StringId.h"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
}

namespace GameEngine::Components
{
struct OceanPresetOverride;
}

namespace GameEngine::Ocean
{

using OceanPresetNumberArray = std::vector<double>;
using OceanPresetValue = std::variant<bool, int64, uint64, double, std::string,
                                      OceanPresetNumberArray>;

class OceanPresetAsset
{
public:
    static constexpr uint32 kFormatVersion = 1u;

    uint64 Revision = 0u;

    void Set(std::string fieldPath, OceanPresetValue value);
    bool Remove(std::string_view fieldPath);
    const OceanPresetValue* Find(std::string_view fieldPath) const;

    bool SaveJson(const std::filesystem::path& path, std::string* error = nullptr) const;
    bool LoadJson(const std::filesystem::path& path, std::string* error = nullptr);

    // Apply every value under `prefix` (for example "Renderer" or "Surface")
    // through the registered component reflection table. A field named by one of
    // the first kOceanPresetOverrideCapacity overrides keeps its scene value; the
    // override names it by HashStringId of the full preset path, prefix included.
    uint32 Apply(std::string_view prefix, ECS::ComponentTypeId typeId,
                 void* component, size_t componentBytes,
                 std::span<const Components::OceanPresetOverride> overrides) const;

private:
    // A preset value and the identity an override names its field by, hashed when
    // the value is set or loaded so Apply compares integers.
    struct Entry
    {
        OceanPresetValue Value;
        StringId FieldIdentifier = 0;
    };

    std::unordered_map<std::string, Entry> m_Values;
};

// The scene overrides of `entity`'s OceanPresetBinding, read in place (the binding
// holds kOceanPresetOverrideCapacity entries, too many to copy per frame); empty when
// the entity has no binding.
std::span<const Components::OceanPresetOverride> PresetOverridesOf(const ECS::World& world,
                                                                   ECS::EntityHandle entity);

class OceanPresetInstance
{
public:
    explicit OceanPresetInstance(std::shared_ptr<const OceanPresetAsset> base = {})
        : m_Base(std::move(base)) {}

    void SetBase(std::shared_ptr<const OceanPresetAsset> base) { m_Base = std::move(base); }
    std::shared_ptr<const OceanPresetAsset> GetBase() const { return m_Base; }
    void SetOverride(std::string fieldPath, OceanPresetValue value);
    bool ResetField(std::string_view fieldPath);
    void ResetAll();
    bool IsOverridden(std::string_view fieldPath) const;
    const OceanPresetValue* Resolve(std::string_view fieldPath) const;

private:
    std::shared_ptr<const OceanPresetAsset> m_Base;
    std::unordered_map<std::string, OceanPresetValue> m_Overrides;
};

} // namespace GameEngine::Ocean
