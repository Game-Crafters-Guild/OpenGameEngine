#pragma once

#include "AssetCore/AssetTypes.h" // AssetType
#include "AssetCore/GUID.h"       // GUID
#include "ECS/Reflection.h"       // FieldTypeTraits, FieldTypeId

#include <type_traits>

namespace GameEngine::Components
{

// A reference to an asset by GUID, for use as a component field.
//
// Holds a GUID directly — GUID is trivially copyable + standard layout (its only member is a
// std::array<uint8,16>; the user-declared constructors don't affect that), so the component stays
// ECS-storable. The template parameter records the expected asset category at compile time: it
// documents intent in code (`MaterialRef material;`) and is the hook for editor asset-pickers +
// load-time type validation. Use `AssetRef<>` for an any-category reference.
//
// Reflects as FieldTypeId::AssetGuid (via the FieldTypeTraits specialization below) — this is the
// "named asset-handle type" that FieldTypeId reserved a slot for. The reflection scene serializer's
// AssetGuid codec round-trips any AssetRef with zero per-field setup (the [path="..." guid="..."]
// form, resolver-healed); the round-trip is category-agnostic, so one codec serves every AssetRef<T>.
template <AssetType kAssetType = AssetType::Unknown>
struct AssetRef
{
    GUID Guid{};

    // The asset category this reference targets (compile-time; not stored per-instance).
    static constexpr AssetType Category = kAssetType;

    AssetRef() = default;
    explicit AssetRef(const GUID& guid) : Guid(guid) {}

    GUID ToGuid() const { return Guid; }
    void Set(const GUID& guid) { Guid = guid; }
    void Clear() { Guid = GUID::Null(); }
    bool IsNull() const { return Guid.IsNull(); }

    bool operator==(const AssetRef& other) const { return Guid == other.Guid; }
    bool operator!=(const AssetRef& other) const { return !(*this == other); }
};

static_assert(std::is_trivially_copyable_v<AssetRef<>>, "AssetRef must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<AssetRef<>>, "AssetRef must be standard layout for ECS storage");

// Friendly aliases for the common categories. Prefer these in component code; use AssetRef<> when
// the category is genuinely open.
using MaterialRef = AssetRef<AssetType::Material>;
using ModelRef    = AssetRef<AssetType::Model>;
using TextureRef  = AssetRef<AssetType::Texture>;
using AudioRef    = AssetRef<AssetType::Audio>;
using OceanDepthCacheRef = AssetRef<AssetType::OceanDepthCache>;
using OceanWaveSpectrumRef = AssetRef<AssetType::OceanWaveSpectrum>;
using OceanFFTCollisionRef = AssetRef<AssetType::OceanFFTCollision>;
using OceanSettingsRef = AssetRef<AssetType::OceanSettings>;
using OceanPresetRef = AssetRef<AssetType::OceanPreset>;
using ParticleStackRef = AssetRef<AssetType::ParticleStack>;

} // namespace GameEngine::Components

namespace GameEngine::ECS
{
// Teach reflection that every AssetRef<T> is an asset GUID, so the AssetGuid field serializer
// handles it. Specializes the ECS FieldTypeTraits customization point (Reflection.h).
template <::GameEngine::AssetType kAssetType>
struct FieldTypeTraits<::GameEngine::Components::AssetRef<kAssetType>>
{
    static constexpr FieldTypeId kType = FieldTypeId::AssetGuid;
};

// Carry the ref's compile-time category into reflection so the editor asset picker
// filters to it generically (no per-component table). Mirrors the trait above.
template <::GameEngine::AssetType kAssetType>
struct FieldAssetCategory<::GameEngine::Components::AssetRef<kAssetType>>
{
    static constexpr std::uint16_t kValue = static_cast<std::uint16_t>(kAssetType);
};
} // namespace GameEngine::ECS
