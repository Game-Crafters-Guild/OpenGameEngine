namespace GameEngine.ComponentScanner;

// One field as a web page sees it: its reflected name and its TypeScript name (camelCase), its
// kind, the enum whose names it takes (null for a non-enum field), its array extent text (null
// when not an array; a String field's extent is its byte capacity), and the markers that reach
// the page.
internal sealed record WebField(string Name, string TsName, FieldKind Kind, EnumDecl? Enum, string? ArrayExtent, bool ReadOnly,
    string? Tooltip, FieldRange? Range);

// NoAdd: the component is marked // @ge-no-add (every entity has one; a page cannot add or remove it).
internal sealed record WebComponent(string Name, bool NoAdd, IReadOnlyList<WebField> Fields);

// Why a field is left out of the web types. NoReflectedKind: its type is a struct declared in the
// scanned headers with no FieldTypeTraits, so the registry reports Unknown too. UnresolvedType:
// its type is not declared in the scanned headers, so the scanner cannot tell what the registry
// reports: an enum declared outside the scanned roots (#3082) or a platform-sized integer (long,
// size_t) has an integer kind there, a struct is Unknown. Omitted therefore means "absent from the
// web types", never "Unknown in the registry".
internal enum WebOmissionReason
{
    NoReflectedKind,
    UnresolvedType,
}

// A field left out of the web types, listed so the gap is counted rather than hidden behind an
// untyped field.
internal sealed record WebOmission(string Component, string Field, string Type, WebOmissionReason Reason);

// The component model the web emitters write (--emit-dts, --emit-json), built from the same
// reflected structs the C++ registration is emitted from. A game export strips editor-only
// components from every scene, so they are left out; so are hidden fields (no page-facing
// surface) and [DoNotSerialize] fields (runtime state the engine rebuilds). A field the scanner
// cannot type is never emitted untyped: it is left out and listed in Omitted. Components are
// sorted by name, fields keep declaration order, Enums lists every enum a kept field uses, by name.
internal sealed class WebComponentModel
{
    public required IReadOnlyList<WebComponent> Components { get; init; }
    public required IReadOnlyList<EnumDecl> Enums { get; init; }
    public required IReadOnlyList<WebOmission> Omitted { get; init; }

    // Builds the model, or returns null with one message per declaration the web types cannot
    // express at all: a multi-dimensional array field, or an enum named like a component (both
    // would be one exported name in components.d.ts). knownStructs holds the simple names of every
    // struct declared in the scanned headers, in any namespace.
    public static WebComponentModel? Build(IEnumerable<ReflectedStruct> reflected, IReadOnlyDictionary<string, string> aliases,
        Dictionary<string, EnumDecl> enums, HashSet<string> ambiguousEnums, IReadOnlySet<string> knownStructs,
        List<string> errors)
    {
        var components = new List<WebComponent>();
        var usedEnums = new SortedDictionary<string, EnumDecl>(StringComparer.Ordinal);
        var omitted = new List<WebOmission>();

        foreach (ReflectedStruct r in reflected.Where(r => !r.EditorOnly).OrderBy(r => r.Name, StringComparer.Ordinal))
        {
            var fields = new List<WebField>();
            foreach (ReflectedField f in r.Fields.Where(f => !f.Hidden && !f.Transient))
            {
                if (f.ArrayExtent != null && f.ArrayExtent.Contains(']'))
                {
                    errors.Add($"{r.Name}.{f.Name} is a multi-dimensional array ([{f.ArrayExtent}]), which the web " +
                        "types cannot express; declare it with one subscript.");
                    continue;
                }
                (FieldKind kind, EnumDecl? enumDecl, string typeName) = FieldKindResolver.Resolve(f, aliases, enums, ambiguousEnums);
                if (kind == FieldKind.Unknown)
                {
                    WebOmissionReason reason = knownStructs.Contains(typeName)
                        ? WebOmissionReason.NoReflectedKind
                        : WebOmissionReason.UnresolvedType;
                    omitted.Add(new WebOmission(r.Name, f.Name, f.Type, reason));
                    continue;
                }
                if (enumDecl != null)
                    usedEnums[enumDecl.Name] = enumDecl;
                fields.Add(new WebField(f.Name, CamelCase(f.Name), kind, enumDecl, f.ArrayExtent, f.ReadOnly, f.Tooltip, f.Range));
            }
            components.Add(new WebComponent(r.Name, r.NoAdd, fields));
        }

        var componentNames = new HashSet<string>(components.Select(c => c.Name), StringComparer.Ordinal);
        foreach (string enumName in usedEnums.Keys.Where(componentNames.Contains))
            errors.Add($"the enum {enumName} and the component {enumName} would share one name in components.d.ts; " +
                "rename one of them.");

        if (errors.Count > 0)
            return null;
        return new WebComponentModel { Components = components, Enums = usedEnums.Values.ToList(), Omitted = omitted };
    }

    // "FovY" -> "fovY", "MeshNameId" -> "meshNameId", "IBLScale" -> "iblScale", "UV" -> "uv":
    // the leading capital run is lowered, keeping the capital that starts the next word.
    private static string CamelCase(string name)
    {
        int run = 0;
        while (run < name.Length && char.IsUpper(name[run]))
            run++;
        if (run == 0)
            return name;
        int lower = run == name.Length || run == 1 ? run : run - 1;
        return name.Substring(0, lower).ToLowerInvariant() + name.Substring(lower);
    }
}
