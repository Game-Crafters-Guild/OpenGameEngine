using System.Text;
using System.Text.RegularExpressions;

namespace GameEngine.ComponentScanner;

// DeclLine/EndLine are the 1-based source line range the field's declaration spans
// (from the first token to its terminating ';'). They let the caller associate a
// field-level marker comment ("// @ge-readonly", "// @ge-hidden"), captured before
// comments were stripped, with the field it annotates. ArrayExtent is the text between the
// first '[' and the last ']' of an array declarator ("3", "kMaxWeights", "kCapacity + 1",
// "4][4" for a two-dimensional array), or null for a non-array field.
internal sealed record FieldDecl(string Type, string Name, int DeclLine = -1, int EndLine = -1, string? ArrayExtent = null);

internal sealed class ParsedStruct
{
    public required string Name { get; init; }
    public required string Namespace { get; init; } // fully-qualified, e.g. "GameEngine::Components"

    // 1-based line number of the "struct X" declaration head in the source. Recorded so the
    // caller can associate a raw-text "// @ge-no-add" marker (captured before comments were
    // stripped) with the struct it precedes. -1 when unknown.
    public int DeclLine { get; init; } = -1;

    public List<FieldDecl> Fields { get; } = new();
    public bool HasVirtual { get; set; }

    // The raw base-clause text (between ':' and '{'), e.g. "public ECS::ComponentBase".
    // Empty when the struct has no bases. Used for inheritance-based detection.
    public string Bases { get; set; } = "";

    // Set when the body contains a bare ALL-CAPS macro invocation (e.g. a
    // GE_*_FIELDS field-injection macro). The scanner does not run the C
    // preprocessor, so fields hidden behind such a macro are invisible; the
    // struct must be skipped rather than emitted with an incomplete field list.
    public string? UnexpandedMacro { get; set; }
}

// Lightweight C++ struct scanner. Operates on comment/literal-stripped source.
// Tracks namespace nesting with a scope stack so each struct gets a fully-qualified
// enclosing namespace, then brace-matches each struct body and extracts top-level
// public data-member field names.
internal static class StructParser
{
    private static readonly Regex s_NamespaceHead =
        new(@"\bnamespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{", RegexOptions.Compiled);

    // struct Name { ... }  or  struct Name : bases { ... }
    // Also matches "class Name {" but we only treat "struct" as a component candidate.
    private static readonly Regex s_StructHead =
        new(@"\b(struct|class)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::([^{};]*))?\{", RegexOptions.Compiled);

    public static List<ParsedStruct> Parse(string src)
    {
        var results = new List<ParsedStruct>();
        ParseScope(src, 0, src.Length, new List<string>(), results);
        return results;
    }

    // Recursively walks [start, end). nsStack is the current namespace path.
    // Top-level structs found in this scope are recorded; their bodies are parsed for
    // fields and also recursively scanned for nested namespaces/structs (the nested ones
    // simply won't carry the GameEngine::Components namespace, so they are ignored later).
    private static void ParseScope(string src, int start, int end, List<string> nsStack, List<ParsedStruct> results)
    {
        int i = start;
        while (i < end)
        {
            char c = src[i];

            if (c == '}')
            {
                // Stray closing brace inside this scope (defensive); skip it and keep going so
                // a malformed region cannot truncate the rest of the namespace.
                i++;
                continue;
            }

            // Try namespace at this position.
            Match nsm = s_NamespaceHead.Match(src, i);
            if (nsm.Success && nsm.Index == FirstNonSpace(src, i, end))
            {
                int bodyOpen = nsm.Index + nsm.Length - 1; // position of '{'
                int bodyClose = MatchBrace(src, bodyOpen, end);
                if (bodyClose < 0) bodyClose = end;

                // A namespace head can itself stack names: "namespace A::B { ... }"
                string nsName = nsm.Groups[1].Value;
                string[] parts = nsName.Split("::", StringSplitOptions.RemoveEmptyEntries);
                int pushed = 0;
                foreach (string p in parts) { nsStack.Add(p); pushed++; }

                ParseScope(src, bodyOpen + 1, bodyClose, nsStack, results);

                for (int k = 0; k < pushed; k++)
                    nsStack.RemoveAt(nsStack.Count - 1);

                i = bodyClose + 1;
                continue;
            }

            // Try struct/class at this position.
            Match sm = s_StructHead.Match(src, i);
            if (sm.Success && sm.Index == FirstNonSpace(src, i, end))
            {
                int bodyOpen = sm.Index + sm.Length - 1; // position of '{'
                int bodyClose = MatchBrace(src, bodyOpen, end);
                if (bodyClose < 0) bodyClose = end;

                string kind = sm.Groups[1].Value;
                string name = sm.Groups[2].Value;

                // Only a non-template "struct" declared directly in a namespace is a component
                // candidate. A `template<...> struct` is a template definition (e.g. AssetRef<T>),
                // not a concrete component — registering it would emit GE_REGISTER_COMPONENT on an
                // unspecialized template, which doesn't compile.
                if (kind == "struct" && !IsTemplatePrefixed(src, sm.Index, start))
                {
                    var ps = new ParsedStruct
                    {
                        Name = name,
                        Namespace = string.Join("::", nsStack),
                        DeclLine = LineNumberAt(src, sm.Index),
                        Bases = sm.Groups[3].Success ? sm.Groups[3].Value.Trim() : "",
                    };
                    ExtractFields(src, bodyOpen + 1, bodyClose, ps);
                    results.Add(ps);
                }

                // Skip past the struct definition entirely. After the closing brace there
                // may be a trailing declarator (e.g. "} foo;") up to the next ';', which we
                // do not treat as a field. Advance to the ';' if present.
                int after = bodyClose + 1;
                int semi = src.IndexOf(';', after);
                i = (semi >= 0 && semi < end) ? semi + 1 : after;
                continue;
            }

            if (c == '{')
            {
                // An unrecognized brace block at namespace scope: a free function body,
                // an extern "C" {...} group, a free initializer, etc. Skip the balanced
                // block wholesale so its contents cannot be mistaken for nested structs and
                // its closing '}' cannot truncate the surrounding namespace scan.
                int close = MatchBrace(src, i, end);
                i = (close < 0) ? end : close + 1;
                continue;
            }

            i++;
        }
    }

    // True if the struct/class keyword at `structIndex` is immediately preceded by a template
    // parameter clause ("template< ... >"), i.e. this is a template definition rather than a
    // concrete type. Walks back over whitespace, a balanced <...>, and checks for the `template`
    // keyword. `lowerBound` clamps the backward walk to the enclosing scope.
    private static bool IsTemplatePrefixed(string src, int structIndex, int lowerBound)
    {
        int j = structIndex - 1;
        while (j >= lowerBound && char.IsWhiteSpace(src[j])) j--;
        if (j < lowerBound || src[j] != '>') return false;

        int depth = 0;
        while (j >= lowerBound)
        {
            char ch = src[j];
            if (ch == '>') depth++;
            else if (ch == '<') { depth--; if (depth == 0) { j--; break; } }
            j--;
        }
        if (depth != 0) return false; // unbalanced angle brackets

        while (j >= lowerBound && char.IsWhiteSpace(src[j])) j--;
        int wordEnd = j + 1;
        while (j >= lowerBound && (char.IsLetterOrDigit(src[j]) || src[j] == '_')) j--;
        return src.Substring(j + 1, wordEnd - (j + 1)) == "template";
    }

    // Extracts top-level member field declarations from a struct body [start, end).
    // Splits on top-level ';' (depth 0 wrt () {} []), skips nested braces, and ignores
    // anything that isn't a plain data member.
    private static void ExtractFields(string src, int start, int end, ParsedStruct ps)
    {
        int i = start;
        var stmt = new StringBuilder();
        // Source index of the first non-whitespace char of the statement currently being
        // accumulated, or -1 when between statements. Used to derive the field's line range.
        int stmtStartIndex = -1;

        while (i < end)
        {
            char c = src[i];

            if (c == '{')
            {
                // Could be a nested type body OR a brace-init "type name{...};".
                // Distinguish: if the accumulated statement so far already has a plausible
                // declarator (identifier before this brace), it's an initializer; else it's
                // a nested definition we must skip.
                string head = stmt.ToString();
                if (LooksLikeMemberInitializer(head))
                {
                    // Brace initializer -> consume balanced braces as part of the statement,
                    // but we don't need its contents; collapse to nothing.
                    int close = MatchBrace(src, i, end);
                    if (close < 0) close = end - 1;
                    i = close + 1;
                    // Keep the head (and stmtStartIndex); the field name was before the '{'.
                    continue;
                }
                else
                {
                    // Nested struct/enum/class/union/anonymous block: skip it and any trailing
                    // declarator up to the next ';'.
                    int close = MatchBrace(src, i, end);
                    if (close < 0) close = end - 1;
                    int after = close + 1;
                    int semi = src.IndexOf(';', after);
                    if (semi < 0 || semi >= end) semi = end - 1;
                    i = semi + 1;
                    stmt.Clear();
                    stmtStartIndex = -1;
                    continue;
                }
            }

            if (c == '(')
            {
                // A '(' that appears AFTER a top-level '=' is part of an initializer
                // expression (e.g. "= static_cast<uint32>(Val)" or "= MakeThing()"),
                // NOT a function parameter list. Consume the balanced parens and keep
                // accumulating the declarator toward its ';' so the field is still
                // extracted — its name/type come from the part before '=', and the
                // initializer is ignored downstream (ParseDeclarator cuts at '=').
                if (HasTopLevelAssign(stmt.ToString()))
                {
                    int initClose = MatchParen(src, i, end);
                    i = (initClose < 0) ? end : initClose + 1;
                    continue;
                }

                // A virtual function is consumed here as a function and never reaches
                // ProcessStatement, so flag it now: a polymorphic struct is non-POD (vtable),
                // not a valid ECS component, and must be skipped by the caller.
                if (Regex.IsMatch(stmt.ToString(), @"\bvirtual\b"))
                    ps.HasVirtual = true;

                // Method / constructor / function-like: consume balanced parens, then
                // skip to the terminating ';' or '{' (function body) — never a field.
                int close = MatchParen(src, i, end);
                if (close < 0) close = end - 1;
                int after = close + 1;

                // Trailing function qualifiers/specifiers can sit between ')' and the body
                // or terminator: const, noexcept, override, final, =default/=delete,
                // ref-qualifiers, trailing-return (-> T). Scan forward to the first '{' or ';'
                // that actually ends the declaration, skipping any such qualifier text.
                int j = after;
                while (j < end && src[j] != '{' && src[j] != ';')
                    j++;

                if (j < end && src[j] == '{')
                {
                    // Function body: skip the balanced braces. A member function body is NOT
                    // followed by a required ';', so resume immediately after the '}' (only
                    // swallow a ';' if it sits adjacent, as after a lambda-assigned member).
                    int bclose = MatchBrace(src, j, end);
                    if (bclose < 0) bclose = end - 1;
                    int afterBody = bclose + 1;
                    int k = afterBody;
                    while (k < end && char.IsWhiteSpace(src[k]))
                        k++;
                    i = (k < end && src[k] == ';') ? k + 1 : afterBody;
                }
                else if (j < end && src[j] == ';')
                {
                    // Declared-only method (no inline body).
                    i = j + 1;
                }
                else
                {
                    i = after;
                }
                stmt.Clear();
                stmtStartIndex = -1;
                continue;
            }

            if (c == ';')
            {
                int startLine = stmtStartIndex >= 0 ? LineNumberAt(src, stmtStartIndex) : LineNumberAt(src, i);
                int endLine = LineNumberAt(src, i);
                ProcessStatement(stmt.ToString(), ps, startLine, endLine);
                stmt.Clear();
                stmtStartIndex = -1;
                i++;
                continue;
            }

            if (stmtStartIndex < 0 && !char.IsWhiteSpace(c))
                stmtStartIndex = i;
            stmt.Append(c);
            i++;
        }

        // Trailing content without a ';' is ignored.
    }

    // True if `head` (text accumulated before a '{') ends in a declarator that would make
    // the following braces an initializer rather than a nested type body. Heuristic:
    // the last token is an identifier or array-subscript and the statement has >= 2 tokens
    // (a type and a name), and does not start with a type-introducing keyword.
    private static bool LooksLikeMemberInitializer(string head)
    {
        string t = head.Trim();
        if (t.Length == 0)
            return false;

        string first = FirstToken(t);
        if (first is "struct" or "class" or "enum" or "union" or "namespace")
            return false;

        // Must contain at least a type and a name (a space-separated boundary), e.g.
        // "float32 Threshold" or "float32 Color[3]". A lone identifier ("Foo") before '{'
        // with no preceding type is a nested type / anonymous block.
        // Count identifier-ish tokens.
        var tokens = Tokenize(t);
        return tokens.Count >= 2;
    }

    // Handle a single ';'-terminated statement: decide skip vs. extract field name(s).
    private static void ProcessStatement(string statement, ParsedStruct ps, int startLine, int endLine)
    {
        string s = statement.Trim();
        if (s.Length == 0)
            return;

        // Detect virtual members anywhere (affects whole-struct skip).
        if (Regex.IsMatch(s, @"\bvirtual\b"))
        {
            ps.HasVirtual = true;
            return;
        }

        // Access specifiers ("public:", "private:", "protected:") arrive with a trailing
        // colon and no ';'. They can also appear as a leftover before the next ';' — strip.
        s = StripAccessSpecifiers(s);
        if (s.Length == 0)
            return;

        string first = FirstToken(s);

        // Skip declarations that are not plain data members.
        switch (first)
        {
            case "static":
            case "using":
            case "typedef":
            case "friend":
            case "enum":
            case "struct":
            case "class":
            case "union":
            case "constexpr":
            case "template":
            case "static_assert":
                return;
        }

        // Function-like (anything still containing '(') — defensive; the paren handler in
        // ExtractFields normally swallows these.
        if (s.Contains('('))
            return;

        // Field-injection macro detection: a bare ALL-CAPS identifier token (no type +
        // name pair following it within this declarator) means an unexpanded macro is
        // pulling in fields the scanner cannot see. Record it so the struct is skipped.
        string? macro = FindBareMacroToken(s);
        if (macro != null)
            ps.UnexpandedMacro = macro;

        // A field type that resolves to a complex type is still recorded; the caller's
        // skip evaluation decides whether to drop the whole struct. We must, however,
        // extract the right field NAME(s) and a representative TYPE string.
        ExtractDeclarators(s, ps, startLine, endLine);
    }

    // Returns the first bare macro-like token (ALL_CAPS with underscores/digits, length >= 4,
    // not a known type keyword) that appears as a leading token of a declarator statement.
    // Such a token can only be an unexpanded function-style-less macro injecting members.
    private static string? FindBareMacroToken(string statement)
    {
        var tokens = Tokenize(statement);
        // Only the leading token(s) before the actual "type name" matter. A real field is
        // "type name [init]"; the final token is the name. Inspect all but the last token.
        int upTo = Math.Max(0, tokens.Count - 1);
        for (int i = 0; i < upTo; i++)
        {
            string t = tokens[i];
            if (IsMacroLikeToken(t))
                return t;
        }
        // A statement that is JUST a bare macro token (no following field, e.g. a macro on its
        // own line that the C preprocessor would expand) also qualifies.
        if (tokens.Count == 1 && IsMacroLikeToken(tokens[0]))
            return tokens[0];
        return null;
    }

    private static bool IsMacroLikeToken(string t)
    {
        if (t.Length < 4)
            return false;
        // Must be all uppercase letters / digits / underscores, contain at least one
        // underscore, start with a letter, and not be a fixed-width-int style alias.
        if (!Regex.IsMatch(t, @"^[A-Z][A-Z0-9_]*$"))
            return false;
        if (!t.Contains('_'))
            return false;
        return true;
    }

    // Parse "type declarator [, declarator]..." into FieldDecls.
    // The declared name is the identifier immediately before '=' , '[' , or end.
    // The type is everything up to the first declarator's name (used for skip checks).
    private static void ExtractDeclarators(string s, ParsedStruct ps, int startLine, int endLine)
    {
        // Drop any initializer: "type a = expr, b = expr" — split declarators on top-level
        // commas, but the type prefix applies to all. First isolate "type" + "first decl".
        // Strategy: split the whole statement on top-level commas. The first chunk carries
        // the type; subsequent chunks are bare declarators sharing that type.
        List<string> chunks = SplitTopLevel(s, ',');
        if (chunks.Count == 0)
            return;

        // Remove initializer from first chunk for type extraction.
        string firstChunk = chunks[0];
        // Extract leading type by removing the trailing declarator (name + optional [..]/=..).
        var firstField = ParseDeclarator(firstChunk, knownType: null, startLine, endLine, out string typePrefix);
        if (firstField == null)
            return;

        // Guard: a statement like "Velocity()" already filtered; but "Camera() = default"
        // would not reach here (paren handler). If typePrefix is empty there was no real type.
        if (typePrefix.Trim().Length == 0)
            return;

        ps.Fields.Add(firstField);

        for (int k = 1; k < chunks.Count; k++)
        {
            var f = ParseDeclarator(chunks[k], knownType: typePrefix, startLine, endLine, out _);
            if (f != null)
                ps.Fields.Add(f);
        }
    }

    // Parse a single declarator chunk. If knownType is null, the type prefix is derived
    // from the chunk (everything before the final name); otherwise knownType is reused
    // (comma-separated declarators share the leading type).
    private static FieldDecl? ParseDeclarator(string chunk, string? knownType, int declLine, int endLine, out string typePrefix)
    {
        typePrefix = knownType ?? "";

        string c = chunk.Trim();
        if (c.Length == 0)
            return null;

        // Cut off initializer: '=' (copy/brace-less) — keep array '[' since it's part of name.
        int eq = IndexOfTopLevel(c, '=');
        string declPart = eq >= 0 ? c.Substring(0, eq).Trim() : c;
        string initPart = eq >= 0 ? c.Substring(eq + 1) : "";

        // Strip array subscript(s) to find the name; record their presence in the name only.
        int bracket = declPart.IndexOf('[');
        string beforeBracket = bracket >= 0 ? declPart.Substring(0, bracket).Trim() : declPart;

        // The name is the last identifier in beforeBracket. The type is everything before it.
        var tokens = Tokenize(beforeBracket);
        if (tokens.Count == 0)
            return null;

        string name = tokens[^1];
        if (!IsIdentifier(name))
            return null;

        if (knownType == null)
        {
            // Type prefix = the declPart text up to (not including) the final name token.
            int nameIdx = beforeBracket.LastIndexOf(name, StringComparison.Ordinal);
            typePrefix = nameIdx > 0 ? beforeBracket.Substring(0, nameIdx).Trim() : "";
            // A bare single token with no type (e.g. "Enabled" alone) means there was no type;
            // treat as not-a-field unless this is a comma continuation (knownType supplied).
            if (typePrefix.Length == 0 && tokens.Count < 2)
                return null;
        }

        // Type string used for skip checks: the type prefix plus any pointer/ref markers
        // that attached to the declarator (e.g. "Foo* p" -> '*' lands in beforeBracket).
        string effectiveType;
        if (knownType != null)
        {
            // Comma continuation ("type a, b"): the leading tokens before the name are the
            // shared type's pointer/ref markers; also pick up '*'/'&' attached to the name.
            string declMarkers = "";
            for (int i = 0; i < tokens.Count - 1; i++)
                declMarkers += tokens[i] + " ";
            effectiveType = knownType + " " + declMarkers + ExtractPointerRefMarkers(beforeBracket);
        }
        else
        {
            effectiveType = typePrefix + " " + ExtractPointerRefMarkers(beforeBracket);
        }

        _ = initPart; // initializer ignored for type/name purposes

        string? arrayExtent = null;
        if (bracket >= 0)
        {
            int close = declPart.LastIndexOf(']');
            if (close > bracket)
                arrayExtent = declPart.Substring(bracket + 1, close - bracket - 1).Trim();
        }

        return new FieldDecl(effectiveType.Trim(), name, declLine, endLine, arrayExtent);
    }

    private static string ExtractPointerRefMarkers(string text)
    {
        var sb = new StringBuilder();
        foreach (char ch in text)
            if (ch == '*' || ch == '&')
                sb.Append(ch);
        return sb.ToString();
    }

    // ---- small lexical helpers ------------------------------------------------

    private static string StripAccessSpecifiers(string s)
    {
        // Remove leading "public:" / "private:" / "protected:" possibly repeated.
        string t = s;
        while (true)
        {
            Match m = Regex.Match(t, @"^\s*(public|private|protected)\s*:");
            if (!m.Success) break;
            t = t.Substring(m.Length);
        }
        return t.Trim();
    }

    private static List<string> Tokenize(string s)
    {
        // Identifiers, ::-qualified names, and standalone */& kept as separate tokens.
        var list = new List<string>();
        var m = Regex.Matches(s, @"[A-Za-z_][A-Za-z0-9_:]*|\*|&");
        foreach (Match x in m)
            list.Add(x.Value);
        return list;
    }

    private static string FirstToken(string s)
    {
        Match m = Regex.Match(s.TrimStart(), @"^[A-Za-z_][A-Za-z0-9_]*");
        return m.Success ? m.Value : "";
    }

    private static bool IsIdentifier(string s) =>
        Regex.IsMatch(s, @"^[A-Za-z_][A-Za-z0-9_]*$");

    // 1-based line number of the character at `index` (count of '\n' before it, + 1).
    // The comment stripper preserves newlines, so a position in the cleaned text maps to the
    // same line number in the raw source.
    private static int LineNumberAt(string src, int index)
    {
        int line = 1;
        int limit = Math.Min(index, src.Length);
        for (int i = 0; i < limit; i++)
            if (src[i] == '\n')
                line++;
        return line;
    }

    private static int FirstNonSpace(string src, int from, int end)
    {
        int i = from;
        while (i < end && char.IsWhiteSpace(src[i]))
            i++;
        return i;
    }

    // Returns index of the matching '}' for the '{' at openIndex, or -1.
    private static int MatchBrace(string src, int openIndex, int end)
    {
        int depth = 0;
        for (int i = openIndex; i < end; i++)
        {
            char c = src[i];
            if (c == '{') depth++;
            else if (c == '}')
            {
                depth--;
                if (depth == 0) return i;
            }
        }
        return -1;
    }

    private static int MatchParen(string src, int openIndex, int end)
    {
        int depth = 0;
        for (int i = openIndex; i < end; i++)
        {
            char c = src[i];
            if (c == '(') depth++;
            else if (c == ')')
            {
                depth--;
                if (depth == 0) return i;
            }
        }
        return -1;
    }

    // Split on a delimiter that is at brace/paren/bracket depth 0.
    private static List<string> SplitTopLevel(string s, char delim)
    {
        var parts = new List<string>();
        int depth = 0;
        var cur = new StringBuilder();
        foreach (char c in s)
        {
            if (c == '(' || c == '{' || c == '[') depth++;
            else if (c == ')' || c == '}' || c == ']') depth--;

            if (c == delim && depth == 0)
            {
                parts.Add(cur.ToString());
                cur.Clear();
            }
            else
            {
                cur.Append(c);
            }
        }
        if (cur.Length > 0 || parts.Count > 0)
            parts.Add(cur.ToString());
        return parts;
    }

    private static int IndexOfTopLevel(string s, char target)
    {
        int depth = 0;
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            if (c == '(' || c == '{' || c == '[') depth++;
            else if (c == ')' || c == '}' || c == ']') depth--;
            else if (c == target && depth == 0)
                return i;
        }
        return -1;
    }

    // True if `s` (the declarator text accumulated before a '(') contains a top-level
    // copy-initializer '=' — i.e. the '(' begins an initializer expression, not a
    // function parameter list. Excludes comparison operators (==, <=, >=, !=) and
    // operator-overload functions (operator=, operator+=, operator(), ...), which are
    // not data members. The accumulated text never contains '(' or '{' (both are
    // consumed by their own handlers), so only '[' ']' affect depth here.
    private static bool HasTopLevelAssign(string s)
    {
        int depth = 0;
        for (int i = 0; i < s.Length; i++)
        {
            char c = s[i];
            if (c == '(' || c == '{' || c == '[') depth++;
            else if (c == ')' || c == '}' || c == ']') depth--;
            else if (c == '=' && depth == 0)
            {
                char prev = i > 0 ? s[i - 1] : '\0';
                char next = i + 1 < s.Length ? s[i + 1] : '\0';
                if (prev == '=' || prev == '<' || prev == '>' || prev == '!' || next == '=')
                    continue; // ==, <=, >=, !=, <=>
                if (Regex.IsMatch(s.Substring(0, i), @"\boperator\s*\S*$"))
                    continue; // operator=, operator+=, operator(), ...
                return true;
            }
        }
        return false;
    }
}
