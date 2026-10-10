using System.Globalization;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace GameEngine.ComponentScanner;

// Writes the component model as JSON, the machine-readable twin of components.d.ts:
//   { "components": [ { "name", "fields": [ { "name", "tsName", "kind", "enum"?, "count"?, "readonly"?,
//                                              "tooltip"?, "min"?, "max"? } ] } ],
//     "enums": [ { "name", "values": [ { "name", "value" } ] } ],
//     "omitted": [ { "component", "field", "type", "reason" } ] }
// "name" is the reflected field name, "tsName" the components.d.ts one. "kind" is the FieldTypeId
// name the registration gets (an enum field reports its underlying integer kind and names its
// enum); "count" is an array's extent, a number when the source writes a literal and the extent
// expression otherwise. Optional keys appear only when set. "omitted" lists the fields left out
// of the web types with their C++ type and why: "noReflectedKind" (the registry reports Unknown
// too) or "unresolvedType" (the scanner cannot tell what the registry reports; see
// WebOmissionReason).
internal static class JsonEmitter
{
    public static string Emit(WebComponentModel model)
    {
        var buffer = new MemoryStream();
        var options = new JsonWriterOptions
        {
            Indented = true,
            NewLine = "\n",
            Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        };
        using (var w = new Utf8JsonWriter(buffer, options))
        {
            w.WriteStartObject();
            w.WriteStartArray("components");
            foreach (WebComponent c in model.Components)
            {
                w.WriteStartObject();
                w.WriteString("name", c.Name);
                w.WriteStartArray("fields");
                foreach (WebField f in c.Fields)
                    WriteField(w, f);
                w.WriteEndArray();
                w.WriteEndObject();
            }
            w.WriteEndArray();

            w.WriteStartArray("enums");
            foreach (EnumDecl e in model.Enums)
            {
                w.WriteStartObject();
                w.WriteString("name", e.Name);
                w.WriteStartArray("values");
                foreach (EnumMember m in e.Members)
                {
                    w.WriteStartObject();
                    w.WriteString("name", m.Name);
                    w.WriteNumber("value", m.Value);
                    w.WriteEndObject();
                }
                w.WriteEndArray();
                w.WriteEndObject();
            }
            w.WriteEndArray();

            w.WriteStartArray("omitted");
            foreach (WebOmission o in model.Omitted)
            {
                w.WriteStartObject();
                w.WriteString("component", o.Component);
                w.WriteString("field", o.Field);
                w.WriteString("type", o.Type);
                w.WriteString("reason", o.Reason == WebOmissionReason.NoReflectedKind ? "noReflectedKind" : "unresolvedType");
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(buffer.ToArray()) + "\n";
    }

    private static void WriteField(Utf8JsonWriter w, WebField f)
    {
        w.WriteStartObject();
        w.WriteString("name", f.Name);
        w.WriteString("tsName", f.TsName);
        w.WriteString("kind", f.Kind.ToString());
        if (f.Enum != null)
            w.WriteString("enum", f.Enum.Name);
        if (f.ArrayExtent != null)
        {
            if (long.TryParse(f.ArrayExtent, NumberStyles.None, CultureInfo.InvariantCulture, out long count))
                w.WriteNumber("count", count);
            else
                w.WriteString("count", f.ArrayExtent);
        }
        if (f.ReadOnly)
            w.WriteBoolean("readonly", true);
        if (f.Tooltip != null)
            w.WriteString("tooltip", f.Tooltip);
        if (f.Range != null)
        {
            w.WriteNumber("min", f.Range.Min);
            if (f.Range.Max.HasValue)
                w.WriteNumber("max", f.Range.Max.Value);
        }
        w.WriteEndObject();
    }
}
