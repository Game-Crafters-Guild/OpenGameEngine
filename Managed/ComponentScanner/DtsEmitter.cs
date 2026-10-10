using System.Globalization;
using System.Text;

namespace GameEngine.ComponentScanner;

// Writes the web library's components.d.ts: per reflected component an exported interface
// (camelCase field names) and a value token `export declare const Name: ComponentType<Name>` that
// entity.get/set/add/remove/has take, intersected with NoAdd for a @ge-no-add component (a page
// reads it but cannot add or remove it); one string union per enum a field uses. ComponentType,
// NoAdd and the view types are imported by name from the library's hand-written opengine.d.ts,
// only the ones the file uses; that file declares ComponentType, NoAdd, Vec3, Color, AssetRef and
// Entity (Vec2, Vec4 and Quat, which no reflected field uses today, would need adding there). An
// entity field is `Entity | null` (an unset handle reads as null). 64-bit integers are bigint, so
// a page never loses bits. A float array whose extent is a literal 2, 3, 4, 9 or 16 is a tuple of
// that length (vectors, 3x3 and 4x4 matrices); other arrays are T[].
internal static class DtsEmitter
{
    private static readonly HashSet<int> TupleExtents = new() { 2, 3, 4, 9, 16 };

    public static string Emit(WebComponentModel model)
    {
        var sb = new StringBuilder();
        sb.Append("// Generated from the engine's component headers by ComponentScanner --emit-dts. Do not edit.\n");
        sb.Append("// One interface and one ComponentType value per reflected component; & NoAdd marks the ones a page\n");
        sb.Append("// cannot add or remove. Each field's type follows its reflected kind.\n");

        var imports = new SortedSet<string>(StringComparer.Ordinal);
        if (model.Components.Count > 0)
            imports.Add("ComponentType");
        if (model.Components.Any(c => c.NoAdd))
            imports.Add("NoAdd");
        foreach (WebComponent c in model.Components)
            foreach (WebField f in c.Fields)
                if (f.Enum == null && ImportedViewType(f.Kind) is string view)
                    imports.Add(view);
        if (imports.Count > 0)
            sb.Append($"\nimport type {{ {string.Join(", ", imports)} }} from './opengine';\n");

        foreach (EnumDecl e in model.Enums)
            sb.Append($"\nexport type {e.Name} = {string.Join(" | ", e.Members.Select(m => $"'{m.Name}'"))};\n");

        foreach (WebComponent c in model.Components)
        {
            sb.Append($"\nexport interface {c.Name} {{\n");
            foreach (WebField f in c.Fields)
            {
                AppendDoc(sb, f);
                sb.Append($"    {(f.ReadOnly ? "readonly " : "")}{f.TsName}: {FieldType(f)};\n");
            }
            sb.Append("}\n");
            sb.Append($"export declare const {c.Name}: ComponentType<{c.Name}>{(c.NoAdd ? " & NoAdd" : "")};\n");
        }
        return sb.ToString();
    }

    private static string FieldType(WebField f)
    {
        if (f.Kind == FieldKind.String)
            return "string";
        string element = f.Enum != null ? f.Enum.Name : ScalarType(f.Kind);
        if (f.Kind == FieldKind.EntityHandle)
            element = f.ArrayExtent == null ? "Entity | null" : "(Entity | null)";
        if (f.ArrayExtent == null)
            return element;
        if (f.Kind == FieldKind.Float && f.Enum == null
            && int.TryParse(f.ArrayExtent, NumberStyles.None, CultureInfo.InvariantCulture, out int extent)
            && TupleExtents.Contains(extent))
            return "[" + string.Join(", ", Enumerable.Repeat(element, extent)) + "]";
        return element + "[]";
    }

    private static string ScalarType(FieldKind kind) => kind switch
    {
        FieldKind.Bool => "boolean",
        FieldKind.Int8 or FieldKind.Int16 or FieldKind.Int32 or FieldKind.UInt8 or FieldKind.UInt16
            or FieldKind.UInt32 or FieldKind.Float or FieldKind.Double => "number",
        FieldKind.Int64 or FieldKind.UInt64 => "bigint",
        _ => ImportedViewType(kind) ?? throw new ArgumentOutOfRangeException(nameof(kind), kind, null),
    };

    private static string? ImportedViewType(FieldKind kind) => kind switch
    {
        FieldKind.Vec2 => "Vec2",
        FieldKind.Vec3 => "Vec3",
        FieldKind.Vec4 => "Vec4",
        FieldKind.Quat => "Quat",
        FieldKind.Color => "Color",
        FieldKind.AssetGuid => "AssetRef",
        FieldKind.EntityHandle => "Entity",
        _ => null,
    };

    // The tooltip, then the range, as the field's JSDoc; no comment for an unmarked field.
    private static void AppendDoc(StringBuilder sb, WebField f)
    {
        var lines = new List<string>();
        if (f.Tooltip != null)
            lines.Add(f.Tooltip.Replace("*/", "*\\/"));
        if (f.Range != null)
            lines.Add(f.Range.Max.HasValue
                ? $"Range: {Number(f.Range.Min)} to {Number(f.Range.Max.Value)}."
                : $"Minimum: {Number(f.Range.Min)}.");
        if (lines.Count == 0)
            return;
        if (lines.Count == 1)
        {
            sb.Append($"    /** {lines[0]} */\n");
            return;
        }
        sb.Append("    /**\n");
        foreach (string line in lines)
            sb.Append($"     * {line}\n");
        sb.Append("     */\n");
    }

    private static string Number(double value) => value.ToString("R", CultureInfo.InvariantCulture);
}
