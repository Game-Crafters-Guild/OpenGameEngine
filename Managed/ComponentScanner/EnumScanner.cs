using System.Globalization;
using System.Text.RegularExpressions;

namespace GameEngine.ComponentScanner;

// One enumerator: its source identifier and integer value.
internal sealed record EnumMember(string Name, long Value);

// A parsed enum declaration: its simple (unqualified) name, ordered enumerators, and the
// declared underlying type text ("uint8", "std::uint32_t"), or null when none is written.
internal sealed record EnumDecl(string Name, IReadOnlyList<EnumMember> Members, string? Underlying = null);

// Discovers C++ `enum` / `enum class` declarations and their enumerator name<->value pairs so the
// scanner can emit a constexpr name table for any enum used by a reflected component field. Runs
// over COMMENT-STRIPPED source (CommentStripper.Strip), so enum bodies carry no comments.
//
// Handles: `enum class`, `enum struct`, plain `enum`; an optional `: underlying` type; implicit
// values (auto-increment from 0) and explicit decimal / hex (0x) / single-char literals; trailing
// commas. Bails on an enum whose any value is a non-literal expression (e.g. `1 << 2`, `A | B`,
// `sizeof(x)`) by dropping it — the caller then leaves that enum's fields as plain integers. That
// is the right outcome for flag enums, which don't have a single serializable name anyway.
internal static class EnumScanner
{
    // enum [class|struct] Name [: underlying] { body }. Enum bodies have no nested braces.
    private static readonly Regex EnumRe = new(
        @"\benum\s+(?:class\s+|struct\s+)?(?<name>[A-Za-z_]\w*)\s*(?::(?<underlying>[^{};]+))?\{(?<body>[^{}]*)\}",
        RegexOptions.Compiled);

    public static List<EnumDecl> Scan(string strippedSource)
    {
        var result = new List<EnumDecl>();
        foreach (Match m in EnumRe.Matches(strippedSource))
        {
            IReadOnlyList<EnumMember>? members = ParseMembers(m.Groups["body"].Value);
            if (members == null || members.Count == 0)
                continue;
            Group underlying = m.Groups["underlying"];
            result.Add(new EnumDecl(m.Groups["name"].Value, members,
                underlying.Success ? underlying.Value.Trim() : null));
        }
        return result;
    }

    // Parse the comma-separated body. Returns null if any enumerator value can't be resolved to an
    // integer literal (the enum is then skipped for name-serialization).
    private static IReadOnlyList<EnumMember>? ParseMembers(string body)
    {
        var members = new List<EnumMember>();
        long next = 0;
        foreach (string raw in SplitTopLevel(body))
        {
            string item = raw.Trim();
            if (item.Length == 0)
                continue; // trailing comma / blank

            int eq = item.IndexOf('=');
            string namePart = (eq < 0 ? item : item.Substring(0, eq)).Trim();
            if (!IsIdentifier(namePart))
                return null; // unexpected token shape — bail on the whole enum

            long value;
            if (eq < 0)
                value = next;
            else if (!TryParseIntLiteral(item.Substring(eq + 1), out value))
                return null; // expression / non-literal value — bail

            members.Add(new EnumMember(namePart, value));
            next = value + 1;
        }
        return members;
    }

    // Split on commas at bracket/paren depth 0 — defensive against a parenthesized value like `(1)`.
    private static IEnumerable<string> SplitTopLevel(string body)
    {
        int depth = 0, start = 0;
        for (int i = 0; i < body.Length; i++)
        {
            char c = body[i];
            if (c is '(' or '[' or '<')
                depth++;
            else if (c is ')' or ']' or '>')
            {
                if (depth > 0) depth--;
            }
            else if (c == ',' && depth == 0)
            {
                yield return body.Substring(start, i - start);
                start = i + 1;
            }
        }
        yield return body.Substring(start);
    }

    private static bool IsIdentifier(string s)
    {
        if (s.Length == 0 || !(char.IsLetter(s[0]) || s[0] == '_'))
            return false;
        foreach (char c in s)
            if (!(char.IsLetterOrDigit(c) || c == '_'))
                return false;
        return true;
    }

    // Parse a C++ integer literal: optional sign, decimal or 0x hex, optional u/l suffixes. Returns
    // false for anything else (an expression we can't evaluate). Char literals never reach here —
    // CommentStripper removes string/char literals before the scanner sees the source.
    private static bool TryParseIntLiteral(string raw, out long value)
    {
        value = 0;
        string s = raw.Trim();
        if (s.Length == 0)
            return false;

        bool neg = false;
        if (s[0] is '+' or '-')
        {
            neg = s[0] == '-';
            s = s.Substring(1).Trim();
        }
        if (s.Length == 0)
            return false;

        // Strip integer suffixes (u, l, ul, ull, ...).
        int end = s.Length;
        while (end > 0 && s[end - 1] is 'u' or 'U' or 'l' or 'L')
            end--;
        string digits = s.Substring(0, end);
        if (digits.Length == 0)
            return false;

        bool ok;
        long parsed;
        if (digits.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            ok = long.TryParse(digits.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out parsed);
        else
            ok = long.TryParse(digits, NumberStyles.Integer, CultureInfo.InvariantCulture, out parsed);
        if (!ok)
            return false;

        value = neg ? -parsed : parsed;
        return true;
    }
}
