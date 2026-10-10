#pragma once

#include "UI/Interaction/Types.h"

#include <any>
#include <string_view>
#include <type_traits>
#include <typeinfo>

namespace GameEngine::UI::Interaction
{
namespace Detail
{
// Deterministic type id: hash a compiler-provided type signature string.
// This avoids global registries/atomics and remains stable across TUs.
consteval PayloadTypeId Fnv1a32(std::string_view s)
{
    std::uint32_t h = 2166136261u;
    for (char c : s)
    {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    // Avoid 0 (reserved for "invalid payload").
    return (h == 0u) ? 1u : h;
}

template<typename T>
consteval std::string_view TypeSig()
{
#if defined(_MSC_VER)
    return std::string_view(__FUNCSIG__);
#else
    return std::string_view(__PRETTY_FUNCTION__);
#endif
}
} // namespace Detail

template<typename T>
inline PayloadTypeId GetPayloadTypeId()
{
    using U = std::decay_t<T>;
    // Hash includes U in the signature string.
    return Detail::Fnv1a32(Detail::TypeSig<U>());
}

inline std::string_view GetPayloadTypeName(PayloadTypeId id)
{
    (void)id;
    return "Unknown";
}

enum class DragGhostIconKind : std::uint8_t
{
    None = 0,
    AssetFile,
    AssetFolder,
    Entity,
};

struct DragPayload
{
    PayloadTypeId TypeId = 0;
    std::any Data;
    std::string DisplayLabel; // optional UI label (ghost), e.g. "Foo + 3"
    DragGhostIconKind GhostIconKind = DragGhostIconKind::None;
    // Optional future extension point (not used yet): provide a custom thumbnail/icon reference.
    // This should remain UI-layer-agnostic (no direct TextureHandle). The overlay may interpret
    // it as an engine background image name or a UI-relative path depending on conventions.
    std::string GhostThumbnailEngineName;

    template<typename T>
    static DragPayload Create(T&& payload)
    {
        using U = std::decay_t<T>;
        DragPayload p;
        p.TypeId = GetPayloadTypeId<U>();
        p.Data = std::forward<T>(payload);
        return p;
    }

    template<typename T>
    const T* TryGet() const
    {
        if (TypeId != GetPayloadTypeId<T>())
            return nullptr;
        return std::any_cast<T>(&Data);
    }

    template<typename T>
    bool Is() const
    {
        return TypeId == GetPayloadTypeId<T>();
    }

    bool IsValid() const { return TypeId != 0; }
};
} // namespace GameEngine::UI::Interaction

