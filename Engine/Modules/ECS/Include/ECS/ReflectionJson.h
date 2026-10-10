#pragma once

// ReflectionJson.h — JSON dump driven by reflected field tables.
//
// Emits {"Field": value, ...} with real field names and decoded values instead of an opaque
// byte blob, for any type carrying a field table. Two entry points: a compile-time one over
// Reflection<T>::Fields, and a runtime one over a ComponentFieldRegistry span for callers that
// only know a ComponentTypeId. Only the runtime tables carry enum name/value bindings, so only
// that form emits enumerator names. Header-only and side-effect-free so it is trivially
// unit-testable.

#include "ECS/Reflection.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace GameEngine {
namespace ECS {

namespace Detail {

template <class T>
T ReadUnaligned(const std::byte* bytes)
{
    T value{};
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

// Locale-independent, round-trip-exact number formatting. std::to_string is
// avoided: it honours the global C locale (a comma decimal separator would emit
// invalid JSON) and formats floats with lossy fixed precision. Non-finite values
// (NaN/Inf) have no JSON literal, so they become null.
template <class T>
void AppendNumber(std::string& out, T value)
{
    if constexpr (std::is_floating_point_v<T>)
    {
        if (!std::isfinite(value)) { out += "null"; return; }
    }
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec == std::errc{})
        out.append(buffer, result.ptr);
    else
        out += "null";
}

inline void AppendHexBytes(std::string& out, const std::byte* bytes, std::uint32_t count)
{
    static constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto b = static_cast<unsigned char>(bytes[i]);
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
    }
    out += '"';
}

inline bool IsAllZero(const std::byte* bytes, std::uint32_t count)
{
    for (std::uint32_t i = 0; i < count; ++i)
        if (bytes[i] != std::byte{0})
            return false;
    return true;
}

// An opaque field — one whose type has no JSON form — as {"size": N, "data": "<hex>"}. The shape
// is what says "raw bytes": a bare hex string is indistinguishable from a GUID at 16 bytes, and
// from an ordinary string at any length.
inline void AppendOpaqueBytes(std::string& out, const std::byte* bytes, std::uint32_t count)
{
    out += "{\"size\":";
    AppendNumber(out, count);
    out += ",\"data\":";
    AppendHexBytes(out, bytes, count);
    out += '}';
}

inline void AppendFloats(std::string& out, const std::byte* bytes, std::uint32_t count)
{
    out += '[';
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (i) out += ',';
        AppendNumber(out, ReadUnaligned<float>(bytes + i * sizeof(float)));
    }
    out += ']';
}

// Append a single value of `type` read from `bytes`. Composite types (vectors,
// quaternion, color, mat4) are emitted as JSON float arrays; a set AssetGuid as a hex string
// and an UNSET one as "", which is the form a writer reads back as "no reference".
inline void AppendScalar(std::string& out, FieldTypeId type, const std::byte* bytes)
{
    switch (type)
    {
        case FieldTypeId::Bool:   out += (ReadUnaligned<bool>(bytes) ? "true" : "false"); break;
        case FieldTypeId::Int8:   AppendNumber(out, static_cast<std::int32_t>(ReadUnaligned<std::int8_t>(bytes))); break;
        case FieldTypeId::Int16:  AppendNumber(out, ReadUnaligned<std::int16_t>(bytes)); break;
        case FieldTypeId::Int32:  AppendNumber(out, ReadUnaligned<std::int32_t>(bytes)); break;
        case FieldTypeId::Int64:  AppendNumber(out, ReadUnaligned<std::int64_t>(bytes)); break;
        case FieldTypeId::UInt8:  AppendNumber(out, static_cast<std::uint32_t>(ReadUnaligned<std::uint8_t>(bytes))); break;
        case FieldTypeId::UInt16: AppendNumber(out, ReadUnaligned<std::uint16_t>(bytes)); break;
        case FieldTypeId::UInt32: AppendNumber(out, ReadUnaligned<std::uint32_t>(bytes)); break;
        case FieldTypeId::UInt64: AppendNumber(out, ReadUnaligned<std::uint64_t>(bytes)); break;
        case FieldTypeId::Float:  AppendNumber(out, ReadUnaligned<float>(bytes)); break;
        case FieldTypeId::Double: AppendNumber(out, ReadUnaligned<double>(bytes)); break;
        case FieldTypeId::EntityHandle: AppendNumber(out, ReadUnaligned<std::uint32_t>(bytes)); break;
        case FieldTypeId::Vec2:   AppendFloats(out, bytes, 2); break;
        case FieldTypeId::Vec3:   AppendFloats(out, bytes, 3); break;
        case FieldTypeId::Vec4:
        case FieldTypeId::Quat:
        case FieldTypeId::Color:  AppendFloats(out, bytes, 4); break;
        case FieldTypeId::Mat4:   AppendFloats(out, bytes, 16); break;
        // An all-zero GUID is an UNSET reference, and its hex form is not a value any writer
        // accepts back (it parses as neither a GUID nor an asset path), so a default component
        // could not be read and written back. "" is what the hand-written asset-ref serializers
        // already emit for unset, and it round-trips.
        case FieldTypeId::AssetGuid:
            if (IsAllZero(bytes, 16))
                out += "\"\"";
            else
                AppendHexBytes(out, bytes, 16);
            break;
        case FieldTypeId::String:
        case FieldTypeId::Unknown:
        case FieldTypeId::Bytes:
        default:                  out += "null"; break;
    }
}

inline void AppendString(std::string& out, const std::byte* bytes, std::uint32_t maxSize)
{
    static constexpr char kHex[] = "0123456789abcdef";
    out += '"';
    for (std::uint32_t i = 0; i < maxSize; ++i)
    {
        const auto uc = static_cast<unsigned char>(bytes[i]);
        if (uc == '\0') break;
        switch (uc)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (uc < 0x20)  // remaining control chars must be \u-escaped (RFC 8259)
                {
                    out += "\\u00";
                    out += kHex[(uc >> 4) & 0x0F];
                    out += kHex[uc & 0x0F];
                }
                else
                {
                    out += static_cast<char>(uc);
                }
                break;
        }
    }
    out += '"';
}

// The integer a scanner-bound enum field holds, widened to the EnumNameValue table's type.
// An enum reflects as its UNDERLYING integer Type, so signedness comes from Type and width
// from Size.
inline std::int64_t ReadEnumValue(const FieldInfo& field, const std::byte* bytes)
{
    const bool isSigned = field.Type == FieldTypeId::Int8 || field.Type == FieldTypeId::Int16 ||
                          field.Type == FieldTypeId::Int32 || field.Type == FieldTypeId::Int64;
    switch (field.Size)
    {
        case 1: return isSigned ? ReadUnaligned<std::int8_t>(bytes)
                                : static_cast<std::int64_t>(ReadUnaligned<std::uint8_t>(bytes));
        case 2: return isSigned ? ReadUnaligned<std::int16_t>(bytes)
                                : static_cast<std::int64_t>(ReadUnaligned<std::uint16_t>(bytes));
        case 4: return isSigned ? ReadUnaligned<std::int32_t>(bytes)
                                : static_cast<std::int64_t>(ReadUnaligned<std::uint32_t>(bytes));
        case 8: return ReadUnaligned<std::int64_t>(bytes);
        default: return 0;
    }
}

inline void AppendFieldValue(std::string& out, const FieldInfo& field, const std::byte* bytes)
{
    if (field.Type == FieldTypeId::String)
    {
        AppendString(out, bytes, field.Size);
        return;
    }

    const std::uint32_t elementSize = FieldElementSize(field.Type);
    if (elementSize == 0)
    {
        AppendOpaqueBytes(out, bytes, field.Size);
        return;
    }

    const std::uint32_t count = field.Size / elementSize;
    if (count == 0)
    {
        // Degenerate field (Size < element size); never read past its storage.
        out += "null";
        return;
    }
    if (count == 1)
    {
        // A scanner-bound enum emits its enumerator NAME, which is the form the scene format and
        // the reflected JSON writers accept back; an out-of-table value falls back to the raw
        // integer so a forward value still round-trips. Enumerator names are C++ identifiers, so
        // they need no JSON escaping. Empty for every field the scanner saw no enum for, and for
        // the constexpr tables (SetFieldEnum binds the table on the registry's copy), so this
        // branch is live exactly for callers driving ComponentFieldRegistry field tables.
        if (!field.EnumNames.empty())
        {
            const std::int64_t value = ReadEnumValue(field, bytes);
            for (const EnumNameValue& e : field.EnumNames)
                if (e.Value == value)
                {
                    out += '"';
                    out.append(e.Name);
                    out += '"';
                    return;
                }
            AppendNumber(out, value);
            return;
        }
        AppendScalar(out, field.Type, bytes);
        return;
    }

    out += '[';
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (i) out += ',';
        AppendScalar(out, field.Type, bytes + i * elementSize);
    }
    out += ']';
}

} // namespace Detail

// Append {"Field": value, ...} for a reflected component to `out`.
template <HasReflection T>
void AppendComponentJson(std::string& out, const T& value)
{
    const auto* base = reinterpret_cast<const std::byte*>(&value);
    out += '{';
    const auto fields = GetReflectedFields<T>();
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (i) out += ',';
        out += '"';
        out.append(fields[i].Name);
        out += "\":";
        Detail::AppendFieldValue(out, fields[i], base + fields[i].Offset);
    }
    out += '}';
}

// Append {"Field": value, ...} for a component described by a RUNTIME field table
// (ComponentFieldRegistry::Get) over a copy of its bytes. The typed overload above is the
// compile-time form; this one serves callers that only know a ComponentTypeId — and it is the
// only form that sees registry-bound enum tables, since SetFieldEnum binds them on the
// registry's copy and never on the constexpr source.
inline void AppendComponentJson(std::string& out, std::span<const FieldInfo> fields,
                                const std::byte* bytes)
{
    out += '{';
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (i) out += ',';
        out += '"';
        out.append(fields[i].Name);
        out += "\":";
        Detail::AppendFieldValue(out, fields[i], bytes + fields[i].Offset);
    }
    out += '}';
}

template <HasReflection T>
std::string ComponentToJson(const T& value)
{
    std::string out;
    AppendComponentJson(out, value);
    return out;
}

} // namespace ECS
} // namespace GameEngine
