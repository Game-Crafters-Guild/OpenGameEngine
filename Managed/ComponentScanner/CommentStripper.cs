using System.Text;

namespace GameEngine.ComponentScanner;

// Removes // line comments, /* */ block comments, and string/char literals from
// C++ source, replacing each removed span with a single space (so token boundaries
// survive). This runs before structural parsing so braces, semicolons, and commas
// hidden inside comments or literals cannot confuse the brace/field tracker.
internal static class CommentStripper
{
    public static string Strip(string src)
    {
        var sb = new StringBuilder(src.Length);
        int i = 0;
        int n = src.Length;

        while (i < n)
        {
            char c = src[i];

            // Line comment
            if (c == '/' && i + 1 < n && src[i + 1] == '/')
            {
                while (i < n && src[i] != '\n')
                    i++;
                continue;
            }

            // Block comment
            if (c == '/' && i + 1 < n && src[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/'))
                {
                    // Preserve newlines so line-based context (if ever used) stays sane.
                    if (src[i] == '\n')
                        sb.Append('\n');
                    i++;
                }
                i += 2; // skip closing */
                sb.Append(' ');
                continue;
            }

            // String literal (handles escapes). Treat raw-string prefixes conservatively
            // as ordinary strings; the engine component headers do not use R"(...)".
            if (c == '"')
            {
                i++;
                while (i < n && src[i] != '"')
                {
                    if (src[i] == '\\' && i + 1 < n)
                        i++;
                    i++;
                }
                i++; // closing quote
                sb.Append(' ');
                continue;
            }

            // Char literal
            if (c == '\'')
            {
                i++;
                while (i < n && src[i] != '\'')
                {
                    if (src[i] == '\\' && i + 1 < n)
                        i++;
                    i++;
                }
                i++; // closing quote
                sb.Append(' ');
                continue;
            }

            sb.Append(c);
            i++;
        }

        return sb.ToString();
    }
}
