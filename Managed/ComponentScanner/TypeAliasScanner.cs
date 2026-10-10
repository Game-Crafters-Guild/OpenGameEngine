using System.Text.RegularExpressions;

namespace GameEngine.ComponentScanner;

// Collects `using Name = Target;` alias declarations from COMMENT-STRIPPED source so a field
// typed through an alias (`MaterialRef`, an alias of `AssetRef<AssetType::Material>`) resolves
// to the kind of its target. Keyed by the alias's simple name; the target is kept as written.
internal static class TypeAliasScanner
{
    private static readonly Regex AliasRe = new(
        @"\busing\s+(?<name>[A-Za-z_]\w*)\s*=\s*(?<target>[^;{}]+);",
        RegexOptions.Compiled);

    public static IEnumerable<(string Name, string Target)> Scan(string strippedSource)
    {
        foreach (Match m in AliasRe.Matches(strippedSource))
            yield return (m.Groups["name"].Value, m.Groups["target"].Value.Trim());
    }
}
