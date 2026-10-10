using System.Text.RegularExpressions;

namespace GameEngine.ComponentScanner;

// Resolves a reflected field's C++ type text to the FieldKind its registration gets from
// FieldTypeIdOf<decltype(T::field)>() (ECS/Reflection.h), without a compiler: the same ladder
// over type names instead of types.
//   - enums       -> the kind of the declared underlying type (int when none is written); the
//                    enumerator table is matched on the name as written (see Resolve)
//   - aliases     -> followed through the `using` declarations of the scanned headers, then the
//                    engine's scalar aliases (Types/Types.h, Types/StringId.h)
//   - arrays      -> the element's kind; a plain char array is String
//   - composites  -> the types that carry a FieldTypeTraits specialization (ColorLinear,
//                    EntityHandle, the Mathematics vectors and quaternion, AssetRef)
//   - otherwise   -> Unknown
internal static class FieldKindResolver
{
    private const int kMaxAliasDepth = 8;

    private static readonly Dictionary<string, FieldKind> KnownTypes = new(StringComparer.Ordinal)
    {
        ["bool"] = FieldKind.Bool,
        ["char"] = FieldKind.Int8,
        ["signed char"] = FieldKind.Int8,
        ["int8_t"] = FieldKind.Int8,
        ["int8"] = FieldKind.Int8,
        ["unsigned char"] = FieldKind.UInt8,
        ["uint8_t"] = FieldKind.UInt8,
        ["uint8"] = FieldKind.UInt8,
        ["byte"] = FieldKind.UInt8,
        ["short"] = FieldKind.Int16,
        ["int16_t"] = FieldKind.Int16,
        ["int16"] = FieldKind.Int16,
        ["unsigned short"] = FieldKind.UInt16,
        ["uint16_t"] = FieldKind.UInt16,
        ["uint16"] = FieldKind.UInt16,
        ["int"] = FieldKind.Int32,
        ["int32_t"] = FieldKind.Int32,
        ["int32"] = FieldKind.Int32,
        ["unsigned"] = FieldKind.UInt32,
        ["unsigned int"] = FieldKind.UInt32,
        ["uint32_t"] = FieldKind.UInt32,
        ["uint32"] = FieldKind.UInt32,
        ["long long"] = FieldKind.Int64,
        ["int64_t"] = FieldKind.Int64,
        ["int64"] = FieldKind.Int64,
        ["unsigned long long"] = FieldKind.UInt64,
        ["uint64_t"] = FieldKind.UInt64,
        ["uint64"] = FieldKind.UInt64,
        ["StringId"] = FieldKind.UInt64,
        ["float"] = FieldKind.Float,
        ["float32"] = FieldKind.Float,
        ["double"] = FieldKind.Double,
        ["float64"] = FieldKind.Double,
        ["Vector2"] = FieldKind.Vec2,
        ["Vector3"] = FieldKind.Vec3,
        ["Vector4"] = FieldKind.Vec4,
        ["Quaternion"] = FieldKind.Quat,
        ["ColorLinear"] = FieldKind.Color,
        ["Color"] = FieldKind.Color,
        ["AssetRef"] = FieldKind.AssetGuid,
        ["EntityHandle"] = FieldKind.EntityHandle,
    };

    private static readonly Regex CvRe = new(@"\b(?:const|volatile|mutable)\b", RegexOptions.Compiled);
    private static readonly Regex SpaceRe = new(@"\s+", RegexOptions.Compiled);

    // The field's kind; for an enum field with a scanned enumerator table, that enum; and the type
    // name the resolution ended on (aliases followed, namespace dropped). The enum table is matched
    // on the name as written, before any alias, as the C++ emitter binds it; an alias that leads to
    // an enum gets the enum's underlying kind and no table, as the registry does.
    public static (FieldKind Kind, EnumDecl? Enum, string TypeName) Resolve(ReflectedField field,
        IReadOnlyDictionary<string, string> aliases, Dictionary<string, EnumDecl> enums, HashSet<string> ambiguousEnums)
    {
        string name = Simplify(field.Type);
        EnumDecl? enumDecl = Program.MatchEnum(name, enums, ambiguousEnums);
        if (enumDecl != null)
            return (ResolveScalar(enumDecl.Underlying ?? "int", aliases), enumDecl, name);

        for (int depth = 0; depth < kMaxAliasDepth && aliases.TryGetValue(name, out string? target); depth++)
            name = Simplify(target);

        if (field.ArrayExtent != null && name == "char")
            return (FieldKind.String, null, name);

        EnumDecl? aliasedEnum = Program.MatchEnum(name, enums, ambiguousEnums);
        if (aliasedEnum != null)
            return (ResolveScalar(aliasedEnum.Underlying ?? "int", aliases), null, name);

        return (ResolveScalar(name, aliases), null, name);
    }

    private static FieldKind ResolveScalar(string typeText, IReadOnlyDictionary<string, string> aliases)
    {
        string name = Simplify(typeText);
        for (int depth = 0; depth < kMaxAliasDepth && aliases.TryGetValue(name, out string? target); depth++)
            name = Simplify(target);
        return KnownTypes.TryGetValue(name, out FieldKind kind) ? kind : FieldKind.Unknown;
    }

    // "const ::GameEngine::Mathematics::Vector3" -> "Vector3"; "std::uint32_t" -> "uint32_t";
    // "AssetRef<AssetType::Model>" -> "AssetRef"; "unsigned   int" -> "unsigned int".
    private static string Simplify(string typeText)
    {
        string t = CvRe.Replace(typeText, " ");
        int lt = t.IndexOf('<');
        if (lt >= 0)
            t = t.Substring(0, lt);
        t = SpaceRe.Replace(t, " ").Trim();
        int ci = t.LastIndexOf("::", StringComparison.Ordinal);
        return ci >= 0 ? t.Substring(ci + 2) : t;
    }
}
