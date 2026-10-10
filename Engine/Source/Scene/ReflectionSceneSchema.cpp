#include "Scene/ReflectionSceneSchema.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "ECS/ComponentFactory.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "Scene/FieldSerializerRegistry.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneValue.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Scene
{
namespace
{
using Rendering::EffectFieldIO;
using Rendering::PostProcessEffectDescriptor;
using Rendering::PostProcessEffectRegistry;

// The .scene parser lowercases component + property names (the format is case-insensitive), but
// reflected field names keep their authored case, so field matching on load must be case-insensitive.
bool NameEquals(std::string_view a, std::string_view b)
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

// A field is serializable as ONE line if it is a single scalar/composite/string (count == 1).
// Genuine C arrays of scalars (Size > one element) and opaque types (Unknown/Bytes, element size 0)
// are skipped — String is the exception: its whole char[] is one value, not an array of chars.
bool IsSerializableField(const ECS::FieldInfo& f)
{
    if (f.Type == ECS::FieldTypeId::String) return true;
    const std::uint32_t elem = ECS::FieldElementSize(f.Type);
    if (elem == 0) return false; // opaque (Unknown / Bytes)
    return f.Size <= elem;       // count == 1 (scalar or composite)
}

// An AssetGuid C-array field (e.g. a ModelRef pool on SplinePlacement): the one array shape the
// reflection schema serializes, one "<Name><index> = [path=... guid=...]" line per non-null
// element. Deliberately NOT generalized to scalar arrays: those are skipped above, and enabling
// them would change the on-disk format of every reflected component carrying float[3]/padding
// array fields — its own change, not a side effect of pools.
bool IsAssetGuidArrayField(const ECS::FieldInfo& f)
{
    if (f.Type != ECS::FieldTypeId::AssetGuid)
        return false;
    const std::uint32_t elem = ECS::FieldElementSize(f.Type);
    return f.Size > elem && (f.Size % elem) == 0;
}

// A C array of a registered struct (FieldInfo::ElementStruct): a fixed-capacity table such as a
// recipe's override list. Each element is written as one "<Name><index>.<SubField> = value" line
// per sub-field, through that sub-field's own codec; a struct array inside an element nests the
// same way ("<Name><index>.<Inner><index>.<SubField>"). An element equal to what the owning
// component's default bytes hold at its place writes no line at all, because that default is
// what the reader starts from — comparing against the element struct's own default instead would
// drop an authored value that happens to equal it wherever the owner defaults differently. So an
// untouched table costs nothing on disk and a filled one round-trips slot-exact, as the AssetGuid
// pools do. A struct with no field table or no default bytes stays opaque and is skipped, as
// every struct array was before.
struct StructArrayLayout
{
    std::span<const ECS::FieldInfo> ElementFields;
    std::uint32_t Stride = 0;
    std::uint32_t Count = 0;
};

bool ResolveStructArray(const ECS::FieldInfo& f, StructArrayLayout& out)
{
    if (f.ElementStruct == 0)
        return false;
    out.ElementFields = ECS::ComponentFieldRegistry::Get(f.ElementStruct);
    std::vector<std::uint8_t> elementDefaults;
    if (out.ElementFields.empty() || !ECS::ComponentFactory::GetDefaultBytes(f.ElementStruct, elementDefaults) ||
        elementDefaults.empty())
        return false;
    const std::size_t stride = elementDefaults.size();
    if (f.Size < stride || f.Size % stride != 0)
        return false;
    out.Stride = static_cast<std::uint32_t>(stride);
    out.Count = static_cast<std::uint32_t>(f.Size / stride);
    return true;
}

// A sub-field one element line can carry: a single value with a codec. A struct array inside an
// element is not one of these; it nests (WriteStructArrayLines).
bool IsSerializableElementField(const ECS::FieldInfo& f)
{
    return !ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Transient) &&
           !ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Hidden) && f.ElementStruct == 0 &&
           IsSerializableField(f);
}

bool IsNestedStructArrayField(const ECS::FieldInfo& f)
{
    return !ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Transient) &&
           !ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Hidden) && f.ElementStruct != 0;
}

// One "suffix = value" line of a struct array, the suffix being everything after the field's
// own key: "3.SpanOrdinal", "1.Leaves0.A".
struct StructArrayLine
{
    std::string Suffix;
    std::string Value;
};

// The lines of one struct-array field whose bytes are `field` and whose owner-default bytes are
// `defaults`, both Size bytes long. Every sub-field of an element that differs from its default
// is written, not just the ones that differ: an authored element then loads the same whatever
// the defaults become later. A struct array nested in an element whose struct has no field table
// cannot be written, and says so rather than vanishing from the file.
void WriteStructArrayLines(const ECS::FieldInfo& f, const std::uint8_t* field, const std::uint8_t* defaults,
                           const SceneSaveContext& ctx, std::string_view componentName, const std::string& prefix,
                           std::vector<StructArrayLine>& out)
{
    StructArrayLayout layout;
    if (!ResolveStructArray(f, layout))
    {
        Logger::Log::Warning("Scene: '{}.{}' holds a struct array whose element type has no reflected field "
                             "table; it is not saved",
                             componentName, prefix);
        return;
    }
    for (std::uint32_t i = 0; i < layout.Count; ++i)
    {
        const std::uint8_t* element = field + static_cast<std::size_t>(i) * layout.Stride;
        const std::uint8_t* elementDefault = defaults + static_cast<std::size_t>(i) * layout.Stride;
        if (std::memcmp(element, elementDefault, layout.Stride) == 0)
            continue;
        const std::string index = prefix + std::to_string(i) + ".";
        for (const ECS::FieldInfo& sub : layout.ElementFields)
        {
            if (sub.Offset + sub.Size > layout.Stride)
                continue;
            if (IsNestedStructArrayField(sub))
            {
                WriteStructArrayLines(sub, element + sub.Offset, elementDefault + sub.Offset, ctx, componentName,
                                      index + std::string(sub.Name), out);
                continue;
            }
            if (!IsSerializableElementField(sub))
                continue;
            const IFieldSerializer* ser = FieldSerializerRegistry::Resolve(sub);
            if (!ser)
                continue;
            std::string value = ser->Write(reinterpret_cast<const std::byte*>(element + sub.Offset), sub, ctx);
            if (value.empty())
                continue;
            out.push_back({index + std::string(sub.Name), std::move(value)});
        }
    }
}

// --- Effect-descriptor metadata (PP-ARCH Phase 1) -------------------------------------------
// A registered PostProcessEffectDescriptor supplies per-field scene-IO metadata the plain
// reflection path cannot express: float[3] encodings (split R/G/B keys or one tuple), load-time
// clamps, enum-as-int on-disk contracts, and legacy key renames. Non-effect components resolve a
// null descriptor and take the plain path untouched.

bool IsFloat3Field(const ECS::FieldInfo& f)
{
    return f.Type == ECS::FieldTypeId::Float && f.Size == 3 * sizeof(float);
}

// The on-disk key for a field: its reflected name unless the descriptor renames it.
std::string_view SerializedKeyFor(const ECS::FieldInfo& f, const EffectFieldIO* io)
{
    return (io && !io->SerializedKey.empty()) ? io->SerializedKey : f.Name;
}

// A SceneValue that may be a Float or an Int (the file writes "1" for a whole-number float).
double EffectSceneValueAsNumber(const SceneValue& v)
{
    if (v.Kind == SceneValueKind::Float) return v.FloatValue;
    if (v.Kind == SceneValueKind::Int) return static_cast<double>(v.IntValue);
    return 0.0;
}

// Read/write a field's underlying integer honoring width + signedness (reflection collapses
// enums to their underlying integer type, so this also serves every reflected enum field).
std::int64_t ReadFieldInt(const std::uint8_t* fieldBytes, const ECS::FieldInfo& f)
{
    const bool isSigned = f.Type == ECS::FieldTypeId::Int8 || f.Type == ECS::FieldTypeId::Int16 ||
                          f.Type == ECS::FieldTypeId::Int32 || f.Type == ECS::FieldTypeId::Int64;
    auto read = [&]<class T>(T) {
        T v{};
        std::memcpy(&v, fieldBytes, sizeof(T));
        return static_cast<std::int64_t>(v);
    };
    switch (f.Size)
    {
    case 1: return isSigned ? read(std::int8_t{}) : read(std::uint8_t{});
    case 2: return isSigned ? read(std::int16_t{}) : read(std::uint16_t{});
    case 4: return isSigned ? read(std::int32_t{}) : read(std::uint32_t{});
    case 8: return read(std::int64_t{});
    default: return 0;
    }
}

void WriteFieldInt(std::uint8_t* fieldBytes, const ECS::FieldInfo& f, std::int64_t v)
{
    switch (f.Size)
    {
    case 1: { const auto x = static_cast<std::uint8_t>(v); std::memcpy(fieldBytes, &x, 1); break; }
    case 2: { const auto x = static_cast<std::uint16_t>(v); std::memcpy(fieldBytes, &x, 2); break; }
    case 4: { const auto x = static_cast<std::uint32_t>(v); std::memcpy(fieldBytes, &x, 4); break; }
    case 8: std::memcpy(fieldBytes, &v, 8); break;
    default: break;
    }
}

bool IsIntegerField(const ECS::FieldInfo& f)
{
    switch (f.Type)
    {
    case ECS::FieldTypeId::Int8:
    case ECS::FieldTypeId::Int16:
    case ECS::FieldTypeId::Int32:
    case ECS::FieldTypeId::Int64:
    case ECS::FieldTypeId::UInt8:
    case ECS::FieldTypeId::UInt16:
    case ECS::FieldTypeId::UInt32:
    case ECS::FieldTypeId::UInt64:
        return true;
    default:
        return false;
    }
}

// Apply the descriptor's load clamp to a just-parsed field. Integer/enum fields honor the
// enum fallback: an out-of-range value snaps to it (a future enum mode) instead of clamping.
void ApplyLoadClamp(std::uint8_t* fieldBytes, const ECS::FieldInfo& f, const EffectFieldIO& io)
{
    if (!io.HasClamp)
        return;
    if (f.Type == ECS::FieldTypeId::Float && f.Size == sizeof(float))
    {
        float v{};
        std::memcpy(&v, fieldBytes, sizeof(float));
        v = std::clamp(v, io.ClampMin, io.ClampMax);
        std::memcpy(fieldBytes, &v, sizeof(float));
        return;
    }
    if (IsIntegerField(f) && f.Size <= 8)
    {
        // Beyond this the float bound is a sentinel (FLT_MAX = unbounded), never a real limit.
        constexpr float kIntBoundLimit = 9.0e18f;
        const std::int64_t lo = io.ClampMin <= -kIntBoundLimit ? std::numeric_limits<std::int64_t>::min()
                                                               : static_cast<std::int64_t>(std::llround(io.ClampMin));
        const std::int64_t hi = io.ClampMax >= kIntBoundLimit ? std::numeric_limits<std::int64_t>::max()
                                                              : static_cast<std::int64_t>(std::llround(io.ClampMax));
        std::int64_t v = ReadFieldInt(fieldBytes, f);
        if (io.HasEnumFallback && (v < lo || v > hi))
            v = io.EnumFallbackValue;
        else
            v = std::clamp(v, lo, hi);
        WriteFieldInt(fieldBytes, f, v);
    }
}

// One property key resolved against the reflected field table + descriptor metadata.
struct ResolvedProperty
{
    const ECS::FieldInfo* Field = nullptr;
    const EffectFieldIO* IO = nullptr;
    int SplitElement = -1; // 0..2 when the key is a split-vector element (e.g. colorG)
    int ArrayIndex = -1;   // element index when the key is "<Name><index>" of an AssetGuid array
                           // or "<Name><index>.<SubField>" of a struct array
    bool DropSilently = false; // a retired legacy key: parse-and-ignore
    // The struct-array sub-field a "<Name><index>.<SubField>" key names (nested arrays walk one
    // more "<Inner><index>." per level), and its byte offset from the start of the component.
    const ECS::FieldInfo* ElementField = nullptr;
    std::uint32_t ElementOffset = 0;
};

// "<Name><index>" split into its name and a bounded decimal index; false when the key does not
// end in digits or is nothing but digits.
bool SplitIndexedKey(std::string_view key, std::string_view& outBase, std::uint32_t& outIndex)
{
    std::size_t digits = 0;
    while (digits < key.size() && std::isdigit(static_cast<unsigned char>(key[key.size() - 1 - digits])))
        ++digits;
    if (digits == 0 || digits > 6 || digits >= key.size())
        return false;
    outBase = key.substr(0, key.size() - digits);
    outIndex = 0;
    for (char c : key.substr(key.size() - digits))
        outIndex = outIndex * 10u + static_cast<std::uint32_t>(c - '0');
    return true;
}

// A "<Name><index>.<SubField>" key, or a nested "<Name><index>.<Inner><index>.<SubField>", against
// the struct-array fields of a component: each "<Field><index>" segment steps into one element,
// and the last segment names a sub-field of the innermost element.
ResolvedProperty ResolveStructArrayElement(ECS::ComponentTypeId typeId, std::string_view property)
{
    std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    const ECS::FieldInfo* topField = nullptr;
    int topIndex = -1;
    std::uint32_t offset = 0;
    std::string_view rest = property;
    for (;;)
    {
        const std::size_t dot = rest.find('.');
        if (dot == std::string_view::npos)
        {
            if (!topField)
                return {};
            for (const ECS::FieldInfo& sub : fields)
            {
                if (NameEquals(sub.Name, rest) && IsSerializableElementField(sub))
                {
                    ResolvedProperty resolved{topField, nullptr, -1, topIndex, false};
                    resolved.ElementField = &sub;
                    resolved.ElementOffset = offset + sub.Offset;
                    return resolved;
                }
            }
            return {};
        }
        std::string_view base;
        std::uint32_t index = 0;
        if (!SplitIndexedKey(rest.substr(0, dot), base, index))
            return {};
        const ECS::FieldInfo* array = nullptr;
        for (const ECS::FieldInfo& f : fields)
        {
            if (f.ElementStruct != 0 && NameEquals(f.Name, base))
            {
                array = &f;
                break;
            }
        }
        StructArrayLayout layout;
        if (!array || !ResolveStructArray(*array, layout) || index >= layout.Count)
            return {};
        if (!topField)
        {
            topField = array;
            topIndex = static_cast<int>(index);
        }
        offset += array->Offset + index * layout.Stride;
        fields = layout.ElementFields;
        rest = rest.substr(dot + 1);
    }
}

ResolvedProperty ResolveProperty(ECS::ComponentTypeId typeId, const PostProcessEffectDescriptor* desc,
                                 std::string_view property)
{
    for (const ECS::FieldInfo& f : ECS::ComponentFieldRegistry::Get(typeId))
    {
        const EffectFieldIO* io = desc ? PostProcessEffectRegistry::FindFieldIO(*desc, f.Name) : nullptr;
        const std::string_view key = SerializedKeyFor(f, io);
        if (io && io->SplitRgb)
        {
            if (property.size() == key.size() + 1 && NameEquals(property.substr(0, key.size()), key))
            {
                static constexpr char kSuffixes[3] = {'r', 'g', 'b'};
                const char last = static_cast<char>(std::tolower(static_cast<unsigned char>(property.back())));
                for (int i = 0; i < 3; ++i)
                {
                    if (last == kSuffixes[i])
                        return {&f, io, i, -1, false};
                }
            }
            continue;
        }
        if (NameEquals(key, property))
            return {&f, io, -1, -1, false};
        // A renamed field still accepts its reflected name (harmless superset of the hand schema).
        if (io && !io->SerializedKey.empty() && NameEquals(f.Name, property))
            return {&f, io, -1, -1, false};
    }

    // Element form "<Name><index>" of an AssetGuid array field (the mirror of the save path;
    // exact field names were tried above, so a field literally named e.g. "Pool2" still wins).
    std::string_view base;
    std::uint32_t index = 0;
    if (SplitIndexedKey(property, base, index))
    {
        for (const ECS::FieldInfo& f : ECS::ComponentFieldRegistry::Get(typeId))
        {
            if (!IsAssetGuidArrayField(f) || !NameEquals(f.Name, base))
                continue;
            const std::uint32_t count = f.Size / ECS::FieldElementSize(f.Type);
            if (index < count)
                return {&f, nullptr, -1, static_cast<int>(index), false};
            break; // right field, out-of-range index: fall through to the unknown-key skip
        }
    }

    // Element sub-field form "<Name><index>.<SubField>" of a struct array field.
    if (const ResolvedProperty element = ResolveStructArrayElement(typeId, property); element.Field)
        return element;

    if (desc)
    {
        for (const std::string_view dropped : desc->DroppedLegacyKeys)
        {
            if (NameEquals(dropped, property))
                return {nullptr, nullptr, -1, -1, true};
        }
    }
    return {};
}

enum class PropertyApply
{
    Applied, // value written into the component bytes
    Skipped, // tolerated no-op (unknown/transient/no-serializer/legacy key)
    Failed   // malformed value; outError set to the bare parse error
};

// One warning for the authored keys a component block names and the component no longer has (renamed
// or removed since the file was written), however many there are.
void WarnUnknownFields(const std::string& componentName, std::span<const std::string_view> unknown)
{
    if (unknown.empty())
        return;
    std::string names;
    for (const std::string_view key : unknown)
        names += (names.empty() ? "'" : ", '") + std::string(key) + "'";
    const bool one = unknown.size() == 1;
    Logger::Log::Warning("Scene: '{}' has no reflected field named {}: {} skipped, and the next save leaves {} out",
                         componentName, names, one ? "its value is" : "their values are", one ? "it" : "them");
}

// Parse `value` into `bytes` for one property. Shared by ApplyProperty and ApplyProperties so
// the skip/error rules cannot drift between the two. On Failed, outError holds the bare error;
// callers prefix "Component.property: ". An authored key the component has no field for keeps the
// default and is appended to `unknownFields`, which the caller reports once per component.
PropertyApply ApplyPropertyToBytes(ECS::ComponentTypeId typeId, const std::string& componentName,
                                   const PostProcessEffectDescriptor* desc, std::vector<std::uint8_t>& bytes,
                                   const SceneLoadContext& ctx, std::string_view property,
                                   std::string_view value, std::string* outError,
                                   std::vector<std::string_view>& unknownFields)
{
    const ResolvedProperty target = ResolveProperty(typeId, desc, property);
    if (target.DropSilently)
        return PropertyApply::Skipped;
    if (!target.Field)
    {
        unknownFields.push_back(property);
        return PropertyApply::Skipped;
    }
    const ECS::FieldInfo& field = *target.Field;
    if (ECS::HasAnyFlag(field.Flags, ECS::FieldFlags::Transient))
        return PropertyApply::Skipped; // runtime-only field — never applied from file
    if (static_cast<std::size_t>(field.Offset) + field.Size > bytes.size())
        return PropertyApply::Skipped; // layout mismatch — skip safely

    std::uint8_t* fieldBytes = bytes.data() + field.Offset;

    if (target.ElementField)
    {
        // One sub-field of one struct-array element, read by the sub-field's own codec. The index
        // was bounds-checked against the array's element count in ResolveProperty.
        const ECS::FieldInfo& sub = *target.ElementField;
        const std::size_t at = target.ElementOffset;
        if (at < field.Offset || at + sub.Size > static_cast<std::size_t>(field.Offset) + field.Size ||
            at + sub.Size > bytes.size())
            return PropertyApply::Skipped; // element layout mismatch — skip safely
        const IFieldSerializer* ser = FieldSerializerRegistry::Resolve(sub);
        if (!ser)
            return PropertyApply::Skipped;
        std::string err;
        if (!ser->Read(value, reinterpret_cast<std::byte*>(bytes.data() + at), sub, ctx, &err))
        {
            if (outError)
                *outError = err;
            return PropertyApply::Failed;
        }
        return PropertyApply::Applied;
    }

    if (target.ArrayIndex >= 0)
    {
        // One element of an AssetGuid array field; the codec reads one element's bytes,
        // so it gets an element-sized view of the field. Index was bounds-checked in
        // ResolveProperty.
        const std::uint32_t elem = ECS::FieldElementSize(field.Type);
        ECS::FieldInfo elemInfo = field;
        elemInfo.Size = elem;
        const IFieldSerializer* ser = FieldSerializerRegistry::Resolve(field);
        if (!ser)
            return PropertyApply::Skipped;
        std::uint8_t* elemBytes = fieldBytes + static_cast<std::size_t>(target.ArrayIndex) * elem;
        std::string err;
        if (!ser->Read(value, reinterpret_cast<std::byte*>(elemBytes), elemInfo, ctx, &err))
        {
            if (outError)
                *outError = err;
            return PropertyApply::Failed;
        }
        return PropertyApply::Applied;
    }

    if (target.SplitElement >= 0)
    {
        if (!IsFloat3Field(field))
            return PropertyApply::Skipped; // descriptor/field drift — skip rather than corrupt
        SceneValue v;
        std::string err;
        if (!ParseValue(value, v, &err) ||
            (v.Kind != SceneValueKind::Float && v.Kind != SceneValueKind::Int))
        {
            if (outError)
                *outError = err.empty() ? "expected number" : err;
            return PropertyApply::Failed;
        }
        float x = static_cast<float>(EffectSceneValueAsNumber(v));
        if (target.IO->HasClamp)
            x = std::clamp(x, target.IO->ClampMin, target.IO->ClampMax);
        std::memcpy(fieldBytes + target.SplitElement * sizeof(float), &x, sizeof(float));
        return PropertyApply::Applied;
    }

    if (target.IO && target.IO->TupleVec3)
    {
        if (!IsFloat3Field(field))
            return PropertyApply::Skipped;
        Float3 out;
        if (!ParseFloat3(value, out))
        {
            if (outError)
                *outError = "expected (x, y, z)";
            return PropertyApply::Failed;
        }
        std::memcpy(fieldBytes, &out.X, sizeof(float));
        std::memcpy(fieldBytes + sizeof(float), &out.Y, sizeof(float));
        std::memcpy(fieldBytes + 2 * sizeof(float), &out.Z, sizeof(float));
        return PropertyApply::Applied;
    }

    const IFieldSerializer* ser = FieldSerializerRegistry::Resolve(field);
    if (!ser)
    {
        Logger::Log::Warning("Scene: '{}.{}' field type has no serializer yet, skipping", componentName,
                             std::string(property));
        return PropertyApply::Skipped;
    }

    std::string err;
    if (!ser->Read(value, reinterpret_cast<std::byte*>(fieldBytes), field, ctx, &err))
    {
        if (outError)
            *outError = err;
        return PropertyApply::Failed;
    }
    if (target.IO)
        ApplyLoadClamp(fieldBytes, field, *target.IO);
    return PropertyApply::Applied;
}
} // namespace

ReflectionSceneSchema::ReflectionSceneSchema(ECS::ComponentTypeId typeId, std::string simpleName)
    : m_TypeId(typeId), m_Name(std::move(simpleName))
{
}

bool ReflectionSceneSchema::IsPresent(const ECS::World& world, ECS::EntityHandle entity) const
{
    return world.HasComponent(entity, m_TypeId);
}

void ReflectionSceneSchema::EnumerateAssetReferences(const ECS::World& world, ECS::EntityHandle entity,
                                                     const AssetRefVisitor& visitor) const
{
    std::vector<std::uint8_t> bytes;
    if (!visitor || !world.HasComponent(entity, m_TypeId) || !world.CaptureComponentBytes(entity, m_TypeId, bytes))
        return;
    for (const ECS::FieldInfo& f : ECS::ComponentFieldRegistry::Get(m_TypeId))
    {
        if (f.Type != ECS::FieldTypeId::AssetGuid || ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Transient) ||
            f.Offset + f.Size > bytes.size())
            continue;
        const std::uint32_t count = f.Size / GUID::kSize;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            std::uint8_t raw[GUID::kSize];
            std::memcpy(raw, bytes.data() + f.Offset + index * GUID::kSize, GUID::kSize);
            const GUID guid = GUID::FromBytes(raw);
            if (guid.IsNull())
                continue;
            const std::string key = count == 1 ? std::string(f.Name) : std::string(f.Name) + std::to_string(index);
            visitor(guid, static_cast<AssetType>(f.AssetCategory), std::string_view{}, key);
        }
    }
}

void ReflectionSceneSchema::Serialize(const ECS::World& world, ECS::EntityHandle entity,
                                      const SceneSaveContext& ctx, std::vector<std::string>& outLines) const
{
    if (!world.HasComponent(entity, m_TypeId))
        return;

    std::vector<std::uint8_t> bytes;
    if (!world.CaptureComponentBytes(entity, m_TypeId, bytes))
        return;

    const PostProcessEffectDescriptor* desc = PostProcessEffectRegistry::Find(m_TypeId);
    // What the reader starts a missing component from; fetched once, and only for a component
    // that has a struct array to compare against it.
    std::vector<std::uint8_t> ownerDefaults;
    bool ownerDefaultsFetched = false;

    const auto emitLine = [&](std::string_view key, std::string_view suffix, const std::string& value)
    {
        // Build "Component.key = value" in place (one allocation), never std::string(f.Name).
        std::string& line = outLines.emplace_back();
        line.reserve(m_Name.size() + key.size() + suffix.size() + value.size() + 5);
        line += m_Name;
        line += '.';
        const std::size_t keyStart = line.size();
        line.append(key);
        // Effect components keep the hand-written schemas' lowerCamel key dialect
        // ("thresholdLsb", "colorR"), so historical scenes resave byte-identical —
        // keys included. Loading is case-insensitive either way.
        if (desc)
            line[keyStart] = static_cast<char>(std::tolower(static_cast<unsigned char>(line[keyStart])));
        line += suffix;
        line += " = ";
        line += value;
    };

    for (const ECS::FieldInfo& f : ECS::ComponentFieldRegistry::Get(m_TypeId))
    {
        if (ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Transient) || ECS::HasAnyFlag(f.Flags, ECS::FieldFlags::Hidden))
            continue;
        if (static_cast<std::size_t>(f.Offset) + f.Size > bytes.size())
            continue;

        const EffectFieldIO* io = desc ? PostProcessEffectRegistry::FindFieldIO(*desc, f.Name) : nullptr;
        if (io && io->SkipSerialize)
            continue; // legacy load-only key
        const std::string_view key = SerializedKeyFor(f, io);

        if (io && (io->SplitRgb || io->TupleVec3))
        {
            if (!IsFloat3Field(f))
                continue; // descriptor/field drift — plain reflection skips float[3] anyway
            float v[3];
            std::memcpy(v, bytes.data() + f.Offset, sizeof(v));
            if (io->SplitRgb)
            {
                static constexpr char kSuffixes[3] = {'R', 'G', 'B'};
                for (int i = 0; i < 3; ++i)
                    emitLine(key, std::string_view(&kSuffixes[i], 1), FormatFloat(v[i]));
            }
            else
            {
                emitLine(key, {}, FormatFloat3(v[0], v[1], v[2]));
            }
            continue;
        }

        if (io && io->EnumAsInt)
        {
            emitLine(key, {}, std::to_string(ReadFieldInt(bytes.data() + f.Offset, f)));
            continue;
        }

        if (f.ElementStruct != 0)
        {
            // The reader starts from the component's default bytes, so an element is compared
            // with those, not with its struct's own default.
            if (!ownerDefaultsFetched)
            {
                ownerDefaultsFetched = true;
                if (!ECS::ComponentFactory::GetDefaultBytes(m_TypeId, ownerDefaults))
                    ownerDefaults.clear();
            }
            if (ownerDefaults.size() != bytes.size())
            {
                Logger::Log::Warning("Scene: '{}' has no default bytes to compare its '{}' table with; "
                                     "the table is not saved",
                                     m_Name, std::string(f.Name));
                continue;
            }
            std::vector<StructArrayLine> lines;
            WriteStructArrayLines(f, bytes.data() + f.Offset, ownerDefaults.data() + f.Offset, ctx, m_Name,
                                  std::string(key), lines);
            for (const StructArrayLine& line : lines)
                emitLine(key, std::string_view(line.Suffix).substr(key.size()), line.Value);
            continue;
        }

        if (IsAssetGuidArrayField(f))
        {
            // One "<Name><index> = [path=... guid=...]" line per non-null element; null
            // slots omit their line and load back as empty (the codec's null-omission
            // rule, element-wise), so a pool round-trips its exact slot layout.
            const IFieldSerializer* ser = FieldSerializerRegistry::Resolve(f);
            if (!ser)
                continue;
            const std::uint32_t elem = ECS::FieldElementSize(f.Type);
            ECS::FieldInfo elemInfo = f;
            elemInfo.Size = elem;
            const std::uint32_t count = f.Size / elem;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const std::byte* elemBytes =
                    reinterpret_cast<const std::byte*>(bytes.data() + f.Offset + i * elem);
                const std::string value = ser->Write(elemBytes, elemInfo, ctx);
                if (value.empty())
                    continue;
                emitLine(key, std::to_string(i), value);
            }
            continue;
        }

        if (!IsSerializableField(f))
            continue;

        const IFieldSerializer* ser = FieldSerializerRegistry::Resolve(f);
        if (!ser)
            continue; // no parser for this field type yet (Mat4/EntityHandle/Bytes) — skipped

        const std::byte* fieldBytes = reinterpret_cast<const std::byte*>(bytes.data() + f.Offset);
        const std::string value = ser->Write(fieldBytes, f, ctx);
        if (value.empty())
            continue; // serializer opted to omit the field (e.g. a null asset reference)
        emitLine(key, {}, value);
    }
}

bool ReflectionSceneSchema::ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                                          std::string_view property, std::string_view value, std::string* outError) const
{
    if (!world.HasComponent(entity, m_TypeId))
    {
        if (!ECS::ComponentFactory::Create(world, entity, m_TypeId))
        {
            if (outError)
                *outError = "cannot create component '" + m_Name + "'";
            return false;
        }
    }

    std::vector<std::uint8_t> bytes;
    if (!world.CaptureComponentBytes(entity, m_TypeId, bytes))
    {
        if (outError)
            *outError = "cannot read component '" + m_Name + "'";
        return false;
    }

    const PostProcessEffectDescriptor* desc = PostProcessEffectRegistry::Find(m_TypeId);
    std::string err;
    std::vector<std::string_view> unknownFields;
    const PropertyApply applied = ApplyPropertyToBytes(m_TypeId, m_Name, desc, bytes, ctx, property, value, &err,
                                                       unknownFields);
    WarnUnknownFields(m_Name, unknownFields);
    if (applied == PropertyApply::Failed)
    {
        if (outError)
            *outError = m_Name + "." + std::string(property) + ": " + err;
        return false;
    }

    if (!world.ApplyComponentBytesImmediate(entity, m_TypeId, bytes))
    {
        if (outError)
            *outError = "cannot apply component '" + m_Name + "'";
        return false;
    }
    return true;
}

bool ReflectionSceneSchema::ApplyProperties(ECS::World& world, ECS::EntityHandle entity, const SceneLoadContext& ctx,
                                            std::span<const std::pair<std::string_view, std::string_view>> props,
                                            std::string* outError, std::size_t* outFailedIndex) const
{
    // Create + capture + apply once for the whole component, instead of per property. Each
    // property's skip/error rules live in ApplyPropertyToBytes, shared with ApplyProperty.
    if (!world.HasComponent(entity, m_TypeId))
    {
        if (!ECS::ComponentFactory::Create(world, entity, m_TypeId))
        {
            if (outError)
                *outError = "cannot create component '" + m_Name + "'";
            return false;
        }
    }

    std::vector<std::uint8_t> bytes;
    if (!world.CaptureComponentBytes(entity, m_TypeId, bytes))
    {
        if (outError)
            *outError = "cannot read component '" + m_Name + "'";
        return false;
    }

    const PostProcessEffectDescriptor* desc = PostProcessEffectRegistry::Find(m_TypeId);
    std::vector<std::string_view> unknownFields;
    for (std::size_t i = 0; i < props.size(); ++i)
    {
        std::string err;
        if (ApplyPropertyToBytes(m_TypeId, m_Name, desc, bytes, ctx, props[i].first, props[i].second, &err,
                                 unknownFields) == PropertyApply::Failed)
        {
            if (outError)
                *outError = m_Name + "." + std::string(props[i].first) + ": " + err;
            if (outFailedIndex)
                *outFailedIndex = i;
            return false;
        }
    }
    WarnUnknownFields(m_Name, unknownFields);

    if (!world.ApplyComponentBytesImmediate(entity, m_TypeId, bytes))
    {
        if (outError)
            *outError = "cannot apply component '" + m_Name + "'";
        return false;
    }
    return true;
}

bool ReflectionSceneSchema::AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* outError) const
{
    if (ECS::ComponentFactory::Create(world, entity, m_TypeId))
        return true;
    if (outError)
        *outError = "cannot create component '" + m_Name + "'";
    return false;
}

bool ReflectionSceneSchema::Remove(ECS::World& world, ECS::EntityHandle entity) const
{
    return world.RemoveComponentByTypeIdImmediate(entity, m_TypeId);
}

} // namespace GameEngine::Scene
