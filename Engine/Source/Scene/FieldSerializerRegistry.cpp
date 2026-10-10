#include "Scene/FieldSerializerRegistry.h"

#include "AssetCore/AssetRegistry.h" // AssetReference, AssetType
#include "AssetCore/GUID.h"
#include "ECS/Entity.h" // ECS::EntityHandle
#include "Scene/SceneIO.h" // FormatAssetReferenceForSave, TryResolveAssetReference, entity-ref helpers
#include "Scene/SceneIOContext.h"
#include "Scene/SceneValue.h"
#include "Types/FormatNumber.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace GameEngine::Scene
{
namespace
{
using ECS::FieldInfo;
using ECS::FieldTypeId;

template <class T>
T ReadPod(const std::byte* p)
{
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <class T>
void WritePod(std::byte* p, T v)
{
    std::memcpy(p, &v, sizeof(T));
}

// A SceneValue that may be a Float or an Int (the file writes "1" for a whole-number float).
double AsNumber(const SceneValue& v)
{
    if (v.Kind == SceneValueKind::Float) return v.FloatValue;
    if (v.Kind == SceneValueKind::Int) return static_cast<double>(v.IntValue);
    return 0.0;
}

std::string_view TrimSpace(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

// FieldTypeIdOf collapsed the enum to its underlying integer's type id, so the enum's signedness
// is its field type's.
bool EnumFieldIsSigned(const FieldInfo& info)
{
    return info.Type == FieldTypeId::Int8 || info.Type == FieldTypeId::Int16 ||
           info.Type == FieldTypeId::Int32 || info.Type == FieldTypeId::Int64;
}

// Read an enum field's underlying integer, honoring its width (FieldInfo::Size) and signedness.
std::int64_t ReadEnumValue(const std::byte* f, const FieldInfo& info)
{
    const bool isSigned = EnumFieldIsSigned(info);
    switch (info.Size)
    {
    case 1: return isSigned ? static_cast<std::int64_t>(ReadPod<std::int8_t>(f))
                            : static_cast<std::int64_t>(ReadPod<std::uint8_t>(f));
    case 2: return isSigned ? static_cast<std::int64_t>(ReadPod<std::int16_t>(f))
                            : static_cast<std::int64_t>(ReadPod<std::uint16_t>(f));
    case 4: return isSigned ? static_cast<std::int64_t>(ReadPod<std::int32_t>(f))
                            : static_cast<std::int64_t>(ReadPod<std::uint32_t>(f));
    case 8: return ReadPod<std::int64_t>(f);
    default: return 0;
    }
}

// Whether an enumerator value survives a write into this field's bytes. WriteEnumValue truncates,
// so an unchecked legacy integer would land as a different enumerator instead of being reported.
bool EnumValueFitsField(const FieldInfo& info, std::int64_t v)
{
    const bool isSigned = EnumFieldIsSigned(info);
    auto fits = [&]<class Signed, class Unsigned>(Signed, Unsigned)
    {
        if (isSigned)
            return v >= std::numeric_limits<Signed>::min() && v <= std::numeric_limits<Signed>::max();
        return v >= 0 && v <= static_cast<std::int64_t>(std::numeric_limits<Unsigned>::max());
    };
    switch (info.Size)
    {
    case 1: return fits(std::int8_t{}, std::uint8_t{});
    case 2: return fits(std::int16_t{}, std::uint16_t{});
    case 4: return fits(std::int32_t{}, std::uint32_t{});
    case 8: return true;
    default: return false;
    }
}

void WriteEnumValue(std::byte* f, const FieldInfo& info, std::int64_t v)
{
    switch (info.Size)
    {
    case 1: WritePod<std::uint8_t>(f, static_cast<std::uint8_t>(v)); break;
    case 2: WritePod<std::uint16_t>(f, static_cast<std::uint16_t>(v)); break;
    case 4: WritePod<std::uint32_t>(f, static_cast<std::uint32_t>(v)); break;
    case 8: WritePod<std::int64_t>(f, v); break;
    default: break;
    }
}

struct BoolSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext&) const override
    {
        return ReadPod<bool>(f) ? "true" : "false";
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext&, std::string* e) const override
    {
        SceneValue v;
        if (!ParseValue(t, v, e) || v.Kind != SceneValueKind::Bool)
        {
            if (e && e->empty()) *e = "expected bool";
            return false;
        }
        WritePod<bool>(f, v.BoolValue);
        return true;
    }
};

template <class T>
struct IntSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext&) const override
    {
        return std::to_string(ReadPod<T>(f));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext&, std::string* e) const override
    {
        // Parse in the field's own domain: SceneValue::IntValue cannot represent UInt64's top bit.
        T value{};
        switch (ParseIntegerToken(t, value))
        {
        case IntegerTokenResult::Ok:
            WritePod<T>(f, value);
            return true;
        case IntegerTokenResult::OutOfRange:
            if (e) *e = "integer out of range";
            return false;
        case IntegerTokenResult::Malformed:
            break;
        }
        if (e) *e = "expected integer";
        return false;
    }
};

struct FloatSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext&) const override
    {
        return GameEngine::FormatFloat(ReadPod<float>(f));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext&, std::string* e) const override
    {
        SceneValue v;
        if (!ParseValue(t, v, e) || (v.Kind != SceneValueKind::Float && v.Kind != SceneValueKind::Int))
        {
            if (e && e->empty()) *e = "expected number";
            return false;
        }
        WritePod<float>(f, static_cast<float>(AsNumber(v)));
        return true;
    }
};

struct DoubleSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext&) const override
    {
        return GameEngine::FormatDouble(ReadPod<double>(f));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext&, std::string* e) const override
    {
        SceneValue v;
        if (!ParseValue(t, v, e) || (v.Kind != SceneValueKind::Float && v.Kind != SceneValueKind::Int))
        {
            if (e && e->empty()) *e = "expected number";
            return false;
        }
        WritePod<double>(f, AsNumber(v));
        return true;
    }
};

struct Vec2Serializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext&) const override
    {
        return FormatFloat2(ReadPod<float>(f), ReadPod<float>(f + 4));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext&, std::string* e) const override
    {
        SceneValue v;
        if (!ParseValue(t, v, e) || v.Kind != SceneValueKind::Tuple || v.Items.size() < 2)
        {
            if (e && e->empty()) *e = "expected (x, y)";
            return false;
        }
        WritePod<float>(f, static_cast<float>(AsNumber(v.Items[0])));
        WritePod<float>(f + 4, static_cast<float>(AsNumber(v.Items[1])));
        return true;
    }
};

struct Vec3Serializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext&) const override
    {
        return FormatFloat3(ReadPod<float>(f), ReadPod<float>(f + 4), ReadPod<float>(f + 8));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext&, std::string* e) const override
    {
        Float3 out;
        if (!ParseFloat3(t, out))
        {
            if (e) *e = "expected (x, y, z)";
            return false;
        }
        WritePod<float>(f, out.X);
        WritePod<float>(f + 4, out.Y);
        WritePod<float>(f + 8, out.Z);
        return true;
    }
};

// Vec4 / Quat / Color all serialize as four floats "(x, y, z, w)".
struct Vec4Serializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext&) const override
    {
        return FormatFloat4(ReadPod<float>(f), ReadPod<float>(f + 4), ReadPod<float>(f + 8), ReadPod<float>(f + 12));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext&, std::string* e) const override
    {
        Float4 out;
        if (!ParseFloat4(t, out))
        {
            if (e) *e = "expected (x, y, z, w)";
            return false;
        }
        WritePod<float>(f, out.X);
        WritePod<float>(f + 4, out.Y);
        WritePod<float>(f + 8, out.Z);
        WritePod<float>(f + 12, out.W);
        return true;
    }
};

// Fixed char[] field: one quoted string, clamped/null-terminated to the field's byte span.
struct StringSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo& info, const SceneSaveContext&) const override
    {
        const char* chars = reinterpret_cast<const char*>(f);
        const std::size_t maxLen = info.Size;
        std::size_t len = 0;
        while (len < maxLen && chars[len] != '\0') ++len;
        return FormatQuoted(std::string_view(chars, len));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo& info, const SceneLoadContext&, std::string* e) const override
    {
        std::string s;
        if (!ParseQuotedString(t, s))
        {
            if (e) *e = "expected quoted string";
            return false;
        }
        const std::size_t cap = info.Size;
        std::memset(f, 0, cap);
        if (cap > 0)
            std::memcpy(f, s.data(), std::min(s.size(), cap - 1));
        return true;
    }
};

// Asset reference field (FieldTypeId::AssetGuid, e.g. a Components::AssetRef<T>): the raw 16 GUID
// bytes <-> the scene's [path="..." guid="..."] form. Save heals the path from the GUID via the
// resolver; load resolves via the [resource]/[embed] tables and accepts the legacy bare-GUID and
// integer-0 clear forms too. A null GUID writes an empty value so the (caller-skipped) line is
// omitted, matching the hand-written asset-ref schemas. Category-agnostic — one codec serves every
// AssetRef<T>; the asset type is only needed for load validation, which we leave permissive here.
struct AssetGuidSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext& ctx) const override
    {
        std::uint8_t raw[GUID::kSize];
        std::memcpy(raw, f, GUID::kSize);
        const GUID guid = GUID::FromBytes(raw);
        if (guid.IsNull())
            return {}; // omit the line for an unset reference
        return FormatAssetReferenceForSave(ctx, guid, "");
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext& ctx, std::string* e) const override
    {
        SceneValue sv{};
        if (!ParseValue(t, sv, e))
            return false;
        AssetReference ref{};
        if (!TryResolveAssetReference(ctx, sv, AssetType::Unknown, ref, e))
            return false;
        std::uint8_t raw[GUID::kSize];
        if (ref.guid.IsNull())
            std::memset(raw, 0, GUID::kSize);
        else
            ref.guid.WriteBytes(raw);
        std::memcpy(f, raw, GUID::kSize);
        return true;
    }
};

// EntityHandle field (FieldTypeId::EntityHandle, e.g. SkyEnvironment.SunLight): the 4-byte handle <->
// a quoted stable entity-id string. Save maps the handle to the referenced entity's scene id via
// ctx.EntityTagById; load resolves the id back to a live handle via ctx.EntityIdMap. An invalid or
// unresolved handle writes an empty value (the line is omitted) and reads back invalid, so a dangling
// link simply unlinks rather than failing the load.
struct EntityHandleSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo&, const SceneSaveContext& ctx) const override
    {
        return FormatEntityReferenceForSave(ctx, ReadPod<ECS::EntityHandle>(f));
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo&, const SceneLoadContext& ctx, std::string* e) const override
    {
        ECS::EntityHandle h{};
        if (!TryResolveEntityReference(ctx, t, h))
        {
            if (e) *e = "expected quoted entity-id string";
            return false;
        }
        WritePod<ECS::EntityHandle>(f, h);
        return true;
    }
};

// Enum field codec: serializes the enumerator NAME from the field's own EnumNames table (bound by
// the scanner — see FieldInfo::EnumNames), not a bare integer. Stateless — the table travels on the
// FieldInfo passed to Write/Read, so one shared instance serves every enum field, no per-field state
// and no allocation. Save: value -> name, or the raw integer if the value isn't in the table
// (forward-compat). Load: name -> value, else parse a legacy integer (pre-name scenes still load).
struct EnumNameSerializer final : IFieldSerializer
{
    std::string Write(const std::byte* f, const FieldInfo& info, const SceneSaveContext&) const override
    {
        const std::int64_t v = ReadEnumValue(f, info);
        for (const ECS::EnumNameValue& e : info.EnumNames)
            if (e.Value == v)
                return std::string(e.Name);
        return std::to_string(v);
    }
    bool Read(std::string_view t, std::byte* f, const FieldInfo& info, const SceneLoadContext&, std::string* e) const override
    {
        const std::string_view name = TrimSpace(t);
        for (const ECS::EnumNameValue& entry : info.EnumNames)
            if (entry.Name == name)
            {
                WriteEnumValue(f, info, entry.Value);
                return true;
            }
        // Dual-read: a legacy (pre-name) or out-of-table integer value.
        SceneValue v;
        if (ParseValue(t, v, nullptr) && v.Kind == SceneValueKind::Int)
        {
            if (!EnumValueFitsField(info, v.IntValue))
            {
                if (e) *e = "enum value out of range";
                return false;
            }
            WriteEnumValue(f, info, v.IntValue);
            return true;
        }
        if (e) *e = "unknown enum value '" + std::string(name) + "'";
        return false;
    }
};

// One file-scope, stateless instance of each serializer — no heap, no ownership. (These are virtual
// types, so not constexpr-constructible, but a single static instance per type is the right
// substitute: they hold no state, reading everything from the FieldInfo/bytes passed to Write/Read.)
const BoolSerializer               kBool;
const IntSerializer<std::int8_t>   kInt8;
const IntSerializer<std::int16_t>  kInt16;
const IntSerializer<std::int32_t>  kInt32;
const IntSerializer<std::int64_t>  kInt64;
const IntSerializer<std::uint8_t>  kUInt8;
const IntSerializer<std::uint16_t> kUInt16;
const IntSerializer<std::uint32_t> kUInt32;
const IntSerializer<std::uint64_t> kUInt64;
const FloatSerializer              kFloat;
const DoubleSerializer             kDouble;
const Vec2Serializer               kVec2;
const Vec3Serializer               kVec3;
const Vec4Serializer               kVec4;  // also serves Quat + Color (four floats)
const StringSerializer             kString;
const AssetGuidSerializer          kAssetGuid;
const EntityHandleSerializer       kEntityHandle;
const EnumNameSerializer           kEnumCodec;

// Flat dispatch table indexed by FieldTypeId — replaces the old hash map + lazy seed + per-call
// mutex. Mat4 / Bytes have no serializer yet (null -> the field is skipped with a warning). The
// order MUST match the FieldTypeId enum; the static_assert guards the count.
const IFieldSerializer* const kDefaults[] = {
    nullptr,      // 0  Unknown
    &kBool,       // 1  Bool
    &kInt8,       // 2  Int8
    &kInt16,      // 3  Int16
    &kInt32,      // 4  Int32
    &kInt64,      // 5  Int64
    &kUInt8,      // 6  UInt8
    &kUInt16,     // 7  UInt16
    &kUInt32,     // 8  UInt32
    &kUInt64,     // 9  UInt64
    &kFloat,      // 10 Float
    &kDouble,     // 11 Double
    &kVec2,       // 12 Vec2
    &kVec3,       // 13 Vec3
    &kVec4,       // 14 Vec4
    &kVec4,       // 15 Quat
    nullptr,      // 16 Mat4
    &kVec4,       // 17 Color
    &kAssetGuid,  // 18 AssetGuid
    &kEntityHandle, // 19 EntityHandle
    &kString,     // 20 String
    nullptr,      // 21 Bytes
};
static_assert(sizeof(kDefaults) / sizeof(kDefaults[0]) == static_cast<std::size_t>(FieldTypeId::Bytes) + 1,
              "kDefaults must have exactly one entry per FieldTypeId");

} // namespace

const IFieldSerializer* FieldSerializerRegistry::Resolve(const FieldInfo& info)
{
    // Scanner-bound enum field: serialize the enumerator name (the table travels on the FieldInfo,
    // so one shared stateless codec handles every enum field).
    if (!info.EnumNames.empty())
        return &kEnumCodec;
    const auto idx = static_cast<std::size_t>(info.Type);
    return idx < (sizeof(kDefaults) / sizeof(kDefaults[0])) ? kDefaults[idx] : nullptr;
}

} // namespace GameEngine::Scene
