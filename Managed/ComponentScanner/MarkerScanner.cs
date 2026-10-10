namespace GameEngine.ComponentScanner;

// 1-based source-line locations of opt-out / field-policy marker comments, captured
// from RAW (un-stripped) source. The comment stripper removes comments before the
// structural parse, so these markers — which live inside "//" line comments — would
// otherwise be invisible. They're harvested up front and associated with structs
// (NoAdd, EditorOnly) or individual fields (ReadOnly / Hidden / Tooltip) afterwards, by line number.
internal sealed class MarkerLines
{
    // Struct-level: "// @ge-no-add" -> register the component but keep it out of the
    // editor's Add Component menu.
    public HashSet<int> NoAdd { get; } = new();

    // Struct-level: "// @ge-editor-only" -> the component saves and loads in the editor, and a
    // game export strips its lines from every staged scene (the entity stays).
    public HashSet<int> EditorOnly { get; } = new();

    // Field-level: "// @ge-readonly" / "// @ge-hidden" -> set FieldFlags::ReadOnly /
    // FieldFlags::Hidden on the annotated field.
    public HashSet<int> ReadOnly { get; } = new();
    public HashSet<int> Hidden { get; } = new();

    // Field-level: "// @ge-tooltip Text" or "// @ge-tooltip \"Text\"" -> set
    // FieldInfo::Tooltip for editor/inspector UI.
    public Dictionary<int, string> Tooltips { get; } = new();

    // Field-level: "// @ge-range Min Max" or "// @ge-range Min" (open maximum) -> bind
    // inspector bounds via GE_REFLECT_FIELD_RANGE. Only the float row consumes them.
    public Dictionary<int, FieldRange> Ranges { get; } = new();

    // Struct- OR field-level: "// [DoNotSerialize]" (aliases: [NonSerializable] / [DontSerialize]
    // and the "@" forms) -> reflected for the inspector but excluded from scene serialization
    // (runtime/derived state: a live GPU handle, a computed world matrix). A struct marker excludes
    // the whole component; a field marker excludes that one field.
    public HashSet<int> DoNotSerialize { get; } = new();
}

// Inspector bounds harvested from "@ge-range". Max is null for an open maximum.
internal sealed record FieldRange(double Min, double? Max);

internal static class MarkerScanner
{
    private const string kNoAdd = "@ge-no-add";
    private const string kEditorOnly = "@ge-editor-only";
    private const string kReadOnly = "@ge-readonly";
    private const string kHidden = "@ge-hidden";
    private const string kTooltip = "@ge-tooltip";
    private const string kRange = "@ge-range";

    // DoNotSerialize marker, matched case-insensitively. The canonical spelling is "[DoNotSerialize]";
    // [NonSerializable] / [DontSerialize] and the "@" forms are accepted aliases.
    private static readonly string[] kDoNotSerialize =
    {
        "[donotserialize]", "[nonserializable]", "[dontserialize]",
        "@donotserialize", "@nonserializable", "@dontserialize",
    };

    // Scan the (1-based-indexable) source lines for marker comments. A marker counts only
    // inside a "//" line comment and is detected regardless of surrounding text (e.g.
    // "// @ge-readonly  runtime handle, not user-editable"). A single line may carry more
    // than one distinct marker; each is recorded independently. The caller splits the raw
    // text once and shares the array with the marker-attachment walk.
    public static MarkerLines Scan(string[] lines)
    {
        var result = new MarkerLines();

        for (int i = 0; i < lines.Length; i++)
        {
            string line = lines[i];
            int slash = line.IndexOf("//", StringComparison.Ordinal);
            if (slash < 0)
                continue;

            int lineNumber = i + 1;
            if (line.IndexOf(kNoAdd, slash, StringComparison.Ordinal) >= 0)
                result.NoAdd.Add(lineNumber);
            if (line.IndexOf(kEditorOnly, slash, StringComparison.Ordinal) >= 0)
                result.EditorOnly.Add(lineNumber);
            if (line.IndexOf(kReadOnly, slash, StringComparison.Ordinal) >= 0)
                result.ReadOnly.Add(lineNumber);
            if (line.IndexOf(kHidden, slash, StringComparison.Ordinal) >= 0)
                result.Hidden.Add(lineNumber);

            int tooltip = line.IndexOf(kTooltip, slash, StringComparison.Ordinal);
            if (tooltip >= 0)
                result.Tooltips[lineNumber] = ParseTooltipText(line.Substring(tooltip + kTooltip.Length));

            int range = line.IndexOf(kRange, slash, StringComparison.Ordinal);
            if (range >= 0)
            {
                FieldRange? parsed = ParseRange(line.Substring(range + kRange.Length));
                if (parsed != null)
                    result.Ranges[lineNumber] = parsed;
                else
                    Console.Error.WriteLine($"warning: malformed @ge-range marker (expected 'Min [Max]'): {line.Trim()}");
            }

            string commentLower = line.Substring(slash).ToLowerInvariant();
            foreach (string alias in kDoNotSerialize)
            {
                if (commentLower.Contains(alias))
                {
                    result.DoNotSerialize.Add(lineNumber);
                    break;
                }
            }
        }

        return result;
    }

    // "Min Max" or "Min" (open maximum), optionally followed by free text. Numbers
    // use invariant culture; a missing Min or a Max below Min is malformed.
    private static FieldRange? ParseRange(string payload)
    {
        string text = payload.Trim();
        if (text.StartsWith(":", StringComparison.Ordinal) || text.StartsWith("=", StringComparison.Ordinal))
            text = text.Substring(1).TrimStart();
        string[] tokens = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        var culture = System.Globalization.CultureInfo.InvariantCulture;
        const System.Globalization.NumberStyles style = System.Globalization.NumberStyles.Float;
        if (tokens.Length < 1 || !double.TryParse(tokens[0], style, culture, out double min) || !double.IsFinite(min))
            return null;
        if (tokens.Length < 2 || !double.TryParse(tokens[1], style, culture, out double max) || !double.IsFinite(max))
            return new FieldRange(min, null);
        return max < min ? null : new FieldRange(min, max);
    }

    private static string ParseTooltipText(string payload)
    {
        string text = payload.Trim();
        if (text.StartsWith(":", StringComparison.Ordinal) || text.StartsWith("=", StringComparison.Ordinal))
            text = text.Substring(1).TrimStart();
        if (text.Length == 0)
            return "";

        if (text[0] != '"')
            return text.Trim();

        var sb = new System.Text.StringBuilder();
        bool escaped = false;
        for (int i = 1; i < text.Length; i++)
        {
            char c = text[i];
            if (escaped)
            {
                sb.Append(c switch
                {
                    '"' => '"',
                    '\\' => '\\',
                    'n' => '\n',
                    'r' => '\r',
                    't' => '\t',
                    _ => c,
                });
                escaped = false;
                continue;
            }

            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            if (c == '"')
                return sb.ToString();
            sb.Append(c);
        }

        return sb.ToString();
    }
}
