namespace GameEngine.ComponentScanner;

// Associates a marker comment (captured by line number in MarkerScanner) with the
// struct or field it annotates. A marker "attaches" when it sits on the declaration's
// own line(s) or on the contiguous comment/blank run immediately preceding it; a line
// of code ends that run, so a marker on a neighbouring declaration never leaks across.
//
// `lines` is the raw source split into 1-based-addressable lines (line N == lines[N-1]),
// shared with MarkerScanner so the split happens once per header.
internal static class MarkerAttachment
{
    // True when a struct-level marker ("// @ge-no-add") attaches to the struct whose
    // declaration head sits on `declLine`: on the declaration line itself, or on the
    // contiguous comment/blank run immediately preceding it.
    public static bool StructMarkerAttached(string[] lines, int declLine, HashSet<int> markerLines)
    {
        if (declLine < 1 || markerLines.Count == 0)
            return false;
        if (markerLines.Contains(declLine))
            return true;
        return InPrecedingCommentRun(lines, declLine, markerLines);
    }

    // True when a field-level marker ("// @ge-readonly" / "// @ge-hidden") attaches to a
    // field whose declaration spans lines [declLine, endLine]: anywhere within that range
    // (covers a trailing "// @ge-..." on the field's line, including the line of a
    // multi-line initializer's terminating ';'), or on the preceding comment/blank run.
    public static bool FieldMarkerAttached(string[] lines, int declLine, int endLine, HashSet<int> markerLines)
    {
        if (markerLines.Count == 0 || declLine < 1)
            return false;

        int last = endLine >= declLine ? endLine : declLine;
        for (int line = declLine; line <= last; line++)
            if (markerLines.Contains(line))
                return true;

        return InPrecedingCommentRun(lines, declLine, markerLines);
    }

    // Returns a field-level marker payload ("// @ge-tooltip Text", "// @ge-range 0 1")
    // attached to a field, using the same line-range / preceding-comment-run rules as
    // FieldMarkerAttached.
    public static TValue? FieldValueMarkerAttached<TValue>(string[] lines, int declLine, int endLine,
        IReadOnlyDictionary<int, TValue> markerLines) where TValue : class
    {
        if (markerLines.Count == 0 || declLine < 1)
            return null;

        int last = endLine >= declLine ? endLine : declLine;
        for (int line = declLine; line <= last; line++)
            if (markerLines.TryGetValue(line, out TValue? value))
                return value;

        return ValueInPrecedingCommentRun(lines, declLine, markerLines);
    }

    // True if a marker line falls within the contiguous comment/blank run immediately above
    // `declLine`. A line of code (not blank, not a pure comment) ends the run.
    private static bool InPrecedingCommentRun(string[] lines, int declLine, HashSet<int> markerLines)
    {
        for (int line = declLine - 1; line >= 1; line--)
        {
            string text = (line - 1 < lines.Length) ? lines[line - 1] : "";
            if (!IsBlankOrCommentLine(text))
                break; // hit code -> attachment run ends
            if (markerLines.Contains(line))
                return true;
        }
        return false;
    }

    private static TValue? ValueInPrecedingCommentRun<TValue>(string[] lines, int declLine,
        IReadOnlyDictionary<int, TValue> markerLines) where TValue : class
    {
        for (int line = declLine - 1; line >= 1; line--)
        {
            string text = (line - 1 < lines.Length) ? lines[line - 1] : "";
            if (!IsBlankOrCommentLine(text))
                break;
            if (markerLines.TryGetValue(line, out TValue? value))
                return value;
        }
        return null;
    }

    // A line that contributes to an "attached leading comment" run: empty/whitespace,
    // or a line whose only non-whitespace content is a "//" or single-line "/* ... */".
    private static bool IsBlankOrCommentLine(string line)
    {
        string t = line.Trim();
        if (t.Length == 0)
            return true;
        if (t.StartsWith("//", StringComparison.Ordinal))
            return true;
        // Multi-line /* */ runs are handled pragmatically: their interior lines start
        // without // so they break the run, which is acceptable since the marker
        // convention is a // line comment.
        if (t.StartsWith("/*", StringComparison.Ordinal) && t.EndsWith("*/", StringComparison.Ordinal))
            return true;
        return false;
    }
}
