#pragma once

// Reflection.h — GE_REFLECT field tables for component types (Phase 2a).
//
// One macro, GE_REFLECT(Type, field, field, ...), emits a compile-time field
// table (Reflection<Type>::Fields) describing each member's name, byte offset,
// byte size, and a FieldTypeId. From that single table the engine derives the
// things that today are hand-written or hex-memcpy'd:
//   * a default property inspector (Phase 3 EditorSDK) that walks the fields,
//   * snapshot serialize/deserialize that survives field add/remove on reload,
//   * a diagnostic JSON dump (see ReflectionJson.h).
//
// Usage — invoke at global/namespace scope with the FULLY-QUALIFIED type name:
//
//     namespace Game { struct Health { float Current = 100.0f; float Max = 100.0f; }; }
//     GE_REFLECT_VERSION(Game::Health, 2);            // optional; defaults to 0
//     GE_REFLECT(Game::Health, Current, Max);
//
// The field-type of each member is deduced via FieldTypeIdOf<decltype(T::field)>().
// Scalars, enums (mapped to their underlying integer), C arrays (mapped to the
// element type with the total byte size carried in FieldInfo::Size), char[]
// (mapped to String), EntityHandle, and Color are handled out of the box.
// Other composite types (math vectors, nested PODs) opt in by specializing
// FieldTypeTraits<T> — see FieldTypeTraits below.

#include "ECS/ComponentTypeName.h"
#include "Types/Color.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace GameEngine {
namespace ECS {

// Forward-declared so the EntityHandle trait can be specialized without pulling
// in the full ECS.h here. Reflecting a component that has an EntityHandle field
// still requires ECS.h at the GE_REFLECT site (for offsetof/sizeof completeness).
struct EntityHandle;

// Element/base type of a reflected field.
//
// These values are a stable taxonomy: APPEND-ONLY, never renumber — once the
// snapshot codec / cross-DLL registrar (Phase 2) persists them they become an
// on-disk/ABI contract. Several values have no producer yet and are the forward
// vocabulary, reachable once their FieldTypeTraits specialization or consumer
// lands: Vec2/Vec4/Quat/Mat4 (await the Mathematics traits — ECS does not link
// Mathematics today), AssetGuid (awaits a named asset-handle type; raw uint8[16]
// fields currently reflect as a UInt8 array, not AssetGuid), and Bytes (opaque
// variable-length blobs).
enum class FieldTypeId : std::uint16_t
{
    Unknown      = 0,
    Bool         = 1,
    Int8         = 2,
    Int16        = 3,
    Int32        = 4,
    Int64        = 5,
    UInt8        = 6,
    UInt16       = 7,
    UInt32       = 8,
    UInt64       = 9,
    Float        = 10,
    Double       = 11,
    Vec2         = 12,
    Vec3         = 13,
    Vec4         = 14,
    Quat         = 15,
    Mat4         = 16,
    Color        = 17,
    AssetGuid    = 18,
    EntityHandle = 19,
    String       = 20,
    Bytes        = 21,
};

// Per-field editor/runtime policy. Orthogonal to FieldTypeId and defaulted to
// None, so it is NOT part of a snapshot/hot-reload match key (the codec matches on
// Name/Size) — adding or changing a flag never affects serialization.
enum class FieldFlags : std::uint16_t
{
    None      = 0,
    Hidden    = 1u << 0,  // omit from the inspector entirely
    ReadOnly  = 1u << 1,  // show but disallow editing; set_component skips it
    Transient = 1u << 2,  // runtime-only: skip in scene serialization (e.g. a live Jolt body handle)
};

constexpr FieldFlags operator|(FieldFlags a, FieldFlags b)
{
    return static_cast<FieldFlags>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}

constexpr bool HasAnyFlag(FieldFlags set, FieldFlags mask)
{
    return (static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(mask)) != 0;
}

// One enumerator of a reflected enum field: its source identifier (the C++ name, e.g.
// "Directional" — NOT a prettied UI label) and integer value. C++ cannot reflect enumerator names
// before C++26, so the build-time ComponentScanner — which sees the enum declaration — emits a
// constexpr table of these and binds it to the field via FieldInfo::EnumNames. The scene serializer
// then writes/reads the enumerator name (with an integer fallback) instead of a bare number.
struct EnumNameValue
{
    std::string_view Name;
    std::int64_t     Value;
};

// One reflected field. Size is the total byte span of the member (for an array
// it is N * sizeof(element)); the element count is Size / element-size. Name is
// a view over a string literal, so Name.data() is a valid C string.
//
// Type alone is NOT a unique field identity: FieldTypeIdOf collapses arrays to
// their element type and enums to their underlying integer, so float[3] and
// float[4], or an enum and a raw uint32, share a Type. A snapshot/hot-reload
// matcher must compare Name AND Size (or element count) — matching on Type alone
// would memcpy a mismatched span when an array's length changes across a reload.
//
// Flags and trailing metadata are defaulted members so the positional
// GE_REFLECT_FIELD_ initializer ({Name, Offset, Size, Type, AssetCategory}) stays
// valid. Optional metadata is applied after registration through ComponentFieldRegistry.
struct FieldInfo
{
    std::string_view Name;
    std::uint32_t    Offset;
    std::uint32_t    Size;
    FieldTypeId      Type;
    // For an AssetRef<C> field, the asset category C as AssetType's underlying value
    // (so the editor asset picker filters to the right category with no per-field
    // table); 0 == AssetType::Unknown for non-asset fields and open AssetRef<>.
    // Populated automatically by GE_REFLECT_FIELD_ from the field's C++ type.
    std::uint16_t    AssetCategory = 0;
    FieldFlags       Flags = FieldFlags::None;
    // Enumerator name<->value table when this field is a (scanner-detected) enum; empty otherwise.
    // A non-owning view over a constexpr table the scanner emits — no allocation, no per-field cost;
    // the serializer/inspector read it straight off the field they already hold.
    std::span<const EnumNameValue> EnumNames = {};
    // Optional inspector slider bounds, applied after registration via
    // ComponentFieldRegistry::SetFieldRange. When HasRange is true the inspector
    // renders a clamped slider over [MinValue, MaxValue] instead of a free drag.
    float            MinValue = 0.0f;
    float            MaxValue = 0.0f;
    bool             HasRange = false;
    // Optional editor/UI tooltip text. Non-owning storage; callers should pass
    // string literals or other storage that outlives the registry entry.
    std::string_view Tooltip = {};
    // For a C array of a struct reflection has no composite for: that struct's
    // component type id, so a consumer can look up the struct's own field table
    // (ComponentFieldRegistry) and default bytes (ComponentFactory). 0 for every
    // other field. Populated automatically by GE_REFLECT_FIELD_; the array is
    // only as readable as the struct's registration makes it, so a struct with
    // no field table stays opaque.
    std::uint64_t ElementStruct = 0;
};

// Customization point for composite field types. The primary template reports
// "not a known composite" (Unknown); FieldTypeIdOf then falls back to the
// scalar/enum/array ladder. Specialize this to teach reflection about a struct
// type (e.g. a math vector) without editing FieldTypeIdOf.
template <class T>
struct FieldTypeTraits
{
    static constexpr FieldTypeId kType = FieldTypeId::Unknown;
};

template <>
struct FieldTypeTraits<ColorLinear>
{
    static constexpr FieldTypeId kType = FieldTypeId::Color;
};

template <>
struct FieldTypeTraits<EntityHandle>
{
    static constexpr FieldTypeId kType = FieldTypeId::EntityHandle;
};

// Map a C++ field type to its FieldTypeId.
//   - arrays      -> element type; total size carried in FieldInfo::Size.
//                    Only PLAIN char[] maps to String — signed char[]/unsigned
//                    char[] (and int8_t[]/uint8_t[], which alias them) are byte
//                    arrays (Int8/UInt8), not text.
//   - enums       -> their underlying integer type
//   - composites  -> FieldTypeTraits<T>::kType when specialized
//   - scalars     -> by signedness + width (robust across char/short/int/long)
//   - otherwise   -> Unknown (consumers treat as opaque bytes)
template <class T>
constexpr FieldTypeId FieldTypeIdOf()
{
    using U = std::remove_cv_t<T>;

    if constexpr (std::is_array_v<U>)
    {
        using Element = std::remove_cv_t<std::remove_extent_t<U>>;
        if constexpr (std::is_same_v<Element, char>)
            return FieldTypeId::String;
        else
            return FieldTypeIdOf<Element>();
    }
    else if constexpr (std::is_enum_v<U>)
    {
        return FieldTypeIdOf<std::underlying_type_t<U>>();
    }
    else if constexpr (FieldTypeTraits<U>::kType != FieldTypeId::Unknown)
    {
        return FieldTypeTraits<U>::kType;
    }
    else if constexpr (std::is_same_v<U, bool>)
    {
        return FieldTypeId::Bool;
    }
    else if constexpr (std::is_same_v<U, float>)
    {
        return FieldTypeId::Float;
    }
    else if constexpr (std::is_same_v<U, double>)
    {
        return FieldTypeId::Double;
    }
    else if constexpr (std::is_integral_v<U>)
    {
        if constexpr (std::is_signed_v<U>)
        {
            if constexpr (sizeof(U) == 1) return FieldTypeId::Int8;
            else if constexpr (sizeof(U) == 2) return FieldTypeId::Int16;
            else if constexpr (sizeof(U) == 4) return FieldTypeId::Int32;
            else return FieldTypeId::Int64;
        }
        else
        {
            if constexpr (sizeof(U) == 1) return FieldTypeId::UInt8;
            else if constexpr (sizeof(U) == 2) return FieldTypeId::UInt16;
            else if constexpr (sizeof(U) == 4) return FieldTypeId::UInt32;
            else return FieldTypeId::UInt64;
        }
    }
    else
    {
        return FieldTypeId::Unknown;
    }
}

// Customization point for AssetRef<C> fields: reports the asset category C (as
// AssetType's underlying value) so the editor asset picker can filter to it. The
// primary template means "not a typed asset ref" (0 == AssetType::Unknown); the
// AssetRef specialization lives in Components/AssetRef.h, next to its
// FieldTypeTraits specialization. This is the generic replacement for any
// per-component asset-type table in the inspector.
template <class T>
struct FieldAssetCategory
{
    static constexpr std::uint16_t kValue = 0;
};

// The component type id of the struct a C array field holds, when reflection
// has no composite for that struct (FieldInfo::ElementStruct); 0 for scalars,
// composites, arrays of either, and a struct field that is not an array.
template <class T>
constexpr std::uint64_t FieldElementStructOf()
{
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_array_v<U>)
    {
        using Element = std::remove_cv_t<std::remove_all_extents_t<U>>;
        if constexpr (std::is_class_v<Element> && FieldTypeIdOf<Element>() == FieldTypeId::Unknown)
            return ComponentTypeHash<Element>();
        else
            return 0;
    }
    else
    {
        return 0;
    }
}

// The asset category of a field type (0 when it isn't an AssetRef). Arrays report
// their element's category; everything else defers to FieldAssetCategory<T>.
template <class T>
constexpr std::uint16_t FieldAssetCategoryOf()
{
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_array_v<U>)
        return FieldAssetCategoryOf<std::remove_cv_t<std::remove_extent_t<U>>>();
    else
        return FieldAssetCategory<U>::kValue;
}

// Byte width of a single element of a FieldTypeId. Used by consumers to derive
// array counts: count = FieldInfo::Size / FieldElementSize(Type).
//
// Returns 0 for opaque types (Unknown / Bytes): consumers MUST treat a 0 result
// as "one Size-byte span" and never divide by it (see ReflectionJson's guard).
constexpr std::uint32_t FieldElementSize(FieldTypeId type)
{
    switch (type)
    {
        case FieldTypeId::Bool:
        case FieldTypeId::Int8:
        case FieldTypeId::UInt8:
        case FieldTypeId::String:      return 1;
        case FieldTypeId::Int16:
        case FieldTypeId::UInt16:      return 2;
        case FieldTypeId::Int32:
        case FieldTypeId::UInt32:
        case FieldTypeId::Float:
        case FieldTypeId::EntityHandle:return 4;
        case FieldTypeId::Int64:
        case FieldTypeId::UInt64:
        case FieldTypeId::Double:      return 8;
        case FieldTypeId::Vec2:        return 8;
        case FieldTypeId::Vec3:        return 12;
        case FieldTypeId::Vec4:        return 16;
        case FieldTypeId::Quat:        return 16;
        case FieldTypeId::Color:       return 16;
        case FieldTypeId::Mat4:        return 64;
        case FieldTypeId::AssetGuid:   return 16;
        case FieldTypeId::Unknown:
        case FieldTypeId::Bytes:
        default:                       return 0;
    }
}

// A composite's hardcoded element size must match the mapped C++ type's layout,
// or array-count derivation silently truncates. Pin each mapped composite here as
// its trait is added. (EntityHandle is pinned by sizeof(EntityHandle)==4 in ECS.h;
// Mathematics vector/quat/mat types get their assert with their traits later.)
static_assert(FieldElementSize(FieldTypeId::Color) == sizeof(ColorLinear),
              "FieldElementSize(Color) must equal sizeof(ColorLinear)");

// Per-type field table. Specialized by GE_REFLECT; the primary template is left
// undefined so HasReflection<T> is false for unreflected types.
template <class T>
struct Reflection;

// Per-type on-disk format version. Specialized by GE_REFLECT_VERSION; defaults
// to 0. GE_REFLECT reads Value into Reflection<T>::Version.
template <class T>
struct ReflectionVersion
{
    static constexpr std::uint32_t Value = 0;
};

// True iff GE_REFLECT(T, ...) has been seen in this translation unit.
template <class T>
concept HasReflection = requires { Reflection<T>::FieldCount; };

template <HasReflection T>
constexpr std::span<const FieldInfo> GetReflectedFields()
{
    return std::span<const FieldInfo>(Reflection<T>::Fields, Reflection<T>::FieldCount);
}

template <HasReflection T>
constexpr std::uint32_t GetReflectionVersion()
{
    return Reflection<T>::Version;
}

// NOTE: the cross-DLL C-ABI mirror (GE_FieldInfo[] + GetCFieldInfos<T>()) is
// intentionally NOT defined yet. It has no consumer until Phase 2's cross-DLL
// component registrar; it will be added then, in this module, wired to that
// registrar — rather than shipped now as an unused export.

} // namespace ECS
} // namespace GameEngine

// ---------------------------------------------------------------------------
// Macro machinery
// ---------------------------------------------------------------------------

// MSVC's traditional preprocessor needs an extra expansion pass for __VA_ARGS__;
// GE_PP_EXPAND provides it and is harmless under the conformant preprocessor.
#define GE_PP_EXPAND(x) x
#define GE_PP_CONCAT(a, b) GE_PP_CONCAT_(a, b)
#define GE_PP_CONCAT_(a, b) a##b

// Count variadic args (1..64).
#define GE_PP_COUNT(...) GE_PP_EXPAND(GE_PP_COUNT_(__VA_ARGS__, GE_PP_COUNT_RSEQ()))
#define GE_PP_COUNT_(...) GE_PP_EXPAND(GE_PP_COUNT_N(__VA_ARGS__))
#define GE_PP_COUNT_N( \
    _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, \
    _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, \
    _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, \
    _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, \
    _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, \
    _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, \
    _61, _62, _63, _64, \
    N, ...) N
#define GE_PP_COUNT_RSEQ() \
    64, 63, 62, 61, 60, 59, 58, 57, 56, 55, \
    54, 53, 52, 51, 50, 49, 48, 47, 46, 45, \
    44, 43, 42, 41, 40, 39, 38, 37, 36, 35, \
    34, 33, 32, 31, 30, 29, 28, 27, 26, 25, \
    24, 23, 22, 21, 20, 19, 18, 17, 16, 15, \
    14, 13, 12, 11, 10, 9, 8, 7, 6, 5, \
    4, 3, 2, 1, 0

// FOR_EACH(macro, ctx, a, b, ...) -> macro(ctx,a) macro(ctx,b) ...
#define GE_PP_FE_1(m, c, a)        m(c, a)
#define GE_PP_FE_2(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_1(m, c, __VA_ARGS__))
#define GE_PP_FE_3(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_2(m, c, __VA_ARGS__))
#define GE_PP_FE_4(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_3(m, c, __VA_ARGS__))
#define GE_PP_FE_5(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_4(m, c, __VA_ARGS__))
#define GE_PP_FE_6(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_5(m, c, __VA_ARGS__))
#define GE_PP_FE_7(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_6(m, c, __VA_ARGS__))
#define GE_PP_FE_8(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_7(m, c, __VA_ARGS__))
#define GE_PP_FE_9(m, c, a, ...)   m(c, a) GE_PP_EXPAND(GE_PP_FE_8(m, c, __VA_ARGS__))
#define GE_PP_FE_10(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_9(m, c, __VA_ARGS__))
#define GE_PP_FE_11(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_10(m, c, __VA_ARGS__))
#define GE_PP_FE_12(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_11(m, c, __VA_ARGS__))
#define GE_PP_FE_13(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_12(m, c, __VA_ARGS__))
#define GE_PP_FE_14(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_13(m, c, __VA_ARGS__))
#define GE_PP_FE_15(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_14(m, c, __VA_ARGS__))
#define GE_PP_FE_16(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_15(m, c, __VA_ARGS__))
#define GE_PP_FE_17(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_16(m, c, __VA_ARGS__))
#define GE_PP_FE_18(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_17(m, c, __VA_ARGS__))
#define GE_PP_FE_19(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_18(m, c, __VA_ARGS__))
#define GE_PP_FE_20(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_19(m, c, __VA_ARGS__))
#define GE_PP_FE_21(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_20(m, c, __VA_ARGS__))
#define GE_PP_FE_22(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_21(m, c, __VA_ARGS__))
#define GE_PP_FE_23(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_22(m, c, __VA_ARGS__))
#define GE_PP_FE_24(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_23(m, c, __VA_ARGS__))
#define GE_PP_FE_25(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_24(m, c, __VA_ARGS__))
#define GE_PP_FE_26(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_25(m, c, __VA_ARGS__))
#define GE_PP_FE_27(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_26(m, c, __VA_ARGS__))
#define GE_PP_FE_28(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_27(m, c, __VA_ARGS__))
#define GE_PP_FE_29(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_28(m, c, __VA_ARGS__))
#define GE_PP_FE_30(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_29(m, c, __VA_ARGS__))
#define GE_PP_FE_31(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_30(m, c, __VA_ARGS__))
#define GE_PP_FE_32(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_31(m, c, __VA_ARGS__))
#define GE_PP_FE_33(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_32(m, c, __VA_ARGS__))
#define GE_PP_FE_34(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_33(m, c, __VA_ARGS__))
#define GE_PP_FE_35(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_34(m, c, __VA_ARGS__))
#define GE_PP_FE_36(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_35(m, c, __VA_ARGS__))
#define GE_PP_FE_37(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_36(m, c, __VA_ARGS__))
#define GE_PP_FE_38(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_37(m, c, __VA_ARGS__))
#define GE_PP_FE_39(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_38(m, c, __VA_ARGS__))
#define GE_PP_FE_40(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_39(m, c, __VA_ARGS__))
#define GE_PP_FE_41(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_40(m, c, __VA_ARGS__))
#define GE_PP_FE_42(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_41(m, c, __VA_ARGS__))
#define GE_PP_FE_43(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_42(m, c, __VA_ARGS__))
#define GE_PP_FE_44(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_43(m, c, __VA_ARGS__))
#define GE_PP_FE_45(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_44(m, c, __VA_ARGS__))
#define GE_PP_FE_46(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_45(m, c, __VA_ARGS__))
#define GE_PP_FE_47(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_46(m, c, __VA_ARGS__))
#define GE_PP_FE_48(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_47(m, c, __VA_ARGS__))
#define GE_PP_FE_49(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_48(m, c, __VA_ARGS__))
#define GE_PP_FE_50(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_49(m, c, __VA_ARGS__))
#define GE_PP_FE_51(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_50(m, c, __VA_ARGS__))
#define GE_PP_FE_52(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_51(m, c, __VA_ARGS__))
#define GE_PP_FE_53(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_52(m, c, __VA_ARGS__))
#define GE_PP_FE_54(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_53(m, c, __VA_ARGS__))
#define GE_PP_FE_55(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_54(m, c, __VA_ARGS__))
#define GE_PP_FE_56(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_55(m, c, __VA_ARGS__))
#define GE_PP_FE_57(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_56(m, c, __VA_ARGS__))
#define GE_PP_FE_58(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_57(m, c, __VA_ARGS__))
#define GE_PP_FE_59(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_58(m, c, __VA_ARGS__))
#define GE_PP_FE_60(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_59(m, c, __VA_ARGS__))
#define GE_PP_FE_61(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_60(m, c, __VA_ARGS__))
#define GE_PP_FE_62(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_61(m, c, __VA_ARGS__))
#define GE_PP_FE_63(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_62(m, c, __VA_ARGS__))
#define GE_PP_FE_64(m, c, a, ...)  m(c, a) GE_PP_EXPAND(GE_PP_FE_63(m, c, __VA_ARGS__))

#define GE_PP_FOR_EACH(m, c, ...) \
    GE_PP_EXPAND(GE_PP_CONCAT(GE_PP_FE_, GE_PP_COUNT(__VA_ARGS__))(m, c, __VA_ARGS__))

// One field entry for the Fields[] initializer. ctx is the (qualified) type name.
#define GE_REFLECT_FIELD_(TypeName, FieldName)                                  \
    ::GameEngine::ECS::FieldInfo{                                               \
        .Name = ::std::string_view(#FieldName),                                \
        .Offset = static_cast<::std::uint32_t>(offsetof(TypeName, FieldName)), \
        .Size = static_cast<::std::uint32_t>(sizeof(TypeName::FieldName)),     \
        .Type = ::GameEngine::ECS::FieldTypeIdOf<decltype(TypeName::FieldName)>(), \
        .AssetCategory =                                                        \
            ::GameEngine::ECS::FieldAssetCategoryOf<decltype(TypeName::FieldName)>(), \
        .ElementStruct =                                                        \
            ::GameEngine::ECS::FieldElementStructOf<decltype(TypeName::FieldName)>() },

// GE_REFLECT(Type, field, field, ...) — define the field table for Type.
//
// Contract:
//   * Invoke at global scope, or inside a namespace that ENCLOSES
//     GameEngine::ECS (in practice: global scope). Invoking it from an unrelated
//     namespace is ill-formed (it defines an explicit specialization of
//     GameEngine::ECS::Reflection) and the compiler error is obscure.
//   * Pass the FULLY-QUALIFIED type name; Type must be complete here (offsetof /
//     sizeof are taken on its members).
//   * 1 to 64 fields. Zero fields is unsupported; 65+ fails to compile with an
//     "undefined GE_PP_FE_<fieldname>" error rather than silently truncating. For
//     components with more fields, use the cap-free GE_REFLECT_BEGIN/FIELD/END form
//     below (the build-time component scanner emits that form).
//   * The caller supplies the trailing ';'.
#define GE_REFLECT(TypeName, ...)                                               \
    template <>                                                                 \
    struct GameEngine::ECS::Reflection<TypeName>                                \
    {                                                                           \
        using ReflectedType = TypeName;                                         \
        static constexpr ::GameEngine::ECS::FieldInfo Fields[] = {              \
            GE_PP_FOR_EACH(GE_REFLECT_FIELD_, TypeName, __VA_ARGS__)            \
        };                                                                      \
        static constexpr ::std::uint32_t FieldCount =                          \
            static_cast<::std::uint32_t>(sizeof(Fields) / sizeof(Fields[0]));   \
        static constexpr ::std::uint32_t Version =                             \
            ::GameEngine::ECS::ReflectionVersion<TypeName>::Value;              \
    }

// GE_REFLECT_VERSION(Type, n) — set Type's on-disk format version (default 0).
// Must precede GE_REFLECT(Type, ...). User supplies the trailing ';'.
#define GE_REFLECT_VERSION(TypeName, VersionNumber)                            \
    template <>                                                                 \
    struct GameEngine::ECS::ReflectionVersion<TypeName>                         \
    {                                                                           \
        static constexpr ::std::uint32_t Value = (VersionNumber);              \
    }

// ---------------------------------------------------------------------------
// Cap-free explicit field-table form: GE_REFLECT_BEGIN / GE_REFLECT_FIELD / GE_REFLECT_END
//
// GE_REFLECT(Type, f1, f2, ...) is bounded by the hand-unrolled FOR_EACH cap
// (currently 64). This form lists one field per line instead of a variadic pack,
// so there is NO arg-count cap — a component can have any number of fields. The
// build-time ComponentScanner emits this form for every reflected component.
//
//     GE_REFLECT_BEGIN(Game::Health)
//         GE_REFLECT_FIELD(Game::Health, Current)
//         GE_REFLECT_FIELD(Game::Health, Max)
//     GE_REFLECT_END(Game::Health);   // caller supplies the trailing ';'
//
// Contract matches GE_REFLECT: invoke at global scope with the fully-qualified
// type name; Type must be complete; at least one field. The three macros must be
// used together and in order (BEGIN opens the table, END closes it).
#define GE_REFLECT_BEGIN(TypeName)                                              \
    template <>                                                                 \
    struct GameEngine::ECS::Reflection<TypeName>                                \
    {                                                                           \
        using ReflectedType = TypeName;                                         \
        static constexpr ::GameEngine::ECS::FieldInfo Fields[] = {

// One field entry; reuses the same FieldInfo initializer GE_REFLECT uses per field
// (trailing comma — a trailing comma after the final entry is valid array-init).
#define GE_REFLECT_FIELD(TypeName, FieldName) GE_REFLECT_FIELD_(TypeName, FieldName)

#define GE_REFLECT_END(TypeName)                                                \
        };                                                                      \
        static constexpr ::std::uint32_t FieldCount =                          \
            static_cast<::std::uint32_t>(sizeof(Fields) / sizeof(Fields[0]));   \
        static constexpr ::std::uint32_t Version =                             \
            ::GameEngine::ECS::ReflectionVersion<TypeName>::Value;              \
    }
