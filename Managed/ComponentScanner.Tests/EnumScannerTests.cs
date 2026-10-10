using System.Collections.Generic;
using System.Linq;
using GameEngine.ComponentScanner;
using Xunit;

namespace GameEngine.ComponentScanner.Tests;

public class EnumScannerTests
{
    private static EnumDecl One(string src, string name) =>
        EnumScanner.Scan(src).Single(e => e.Name == name);

    [Fact]
    public void ParsesImplicitAndExplicitValues()
    {
        var e = One("enum class Mode : uint32 { Off = 0, Low = 1, High = 2, Turbo = 7 };", "Mode");
        Assert.Equal(4, e.Members.Count);
        Assert.Equal(("Off", 0L), (e.Members[0].Name, e.Members[0].Value));
        Assert.Equal(("Turbo", 7L), (e.Members[3].Name, e.Members[3].Value));
    }

    [Fact]
    public void ImplicitValuesAutoIncrementFromPrevious()
    {
        // Mix: implicit 0, explicit 5, then implicit 6, 7.
        var e = One("enum class E { A, B = 5, C, D };", "E");
        Assert.Equal(new long[] { 0, 5, 6, 7 }, e.Members.Select(m => m.Value));
        Assert.Equal(new[] { "A", "B", "C", "D" }, e.Members.Select(m => m.Name));
    }

    [Fact]
    public void ParsesHexValues()
    {
        var e = One("enum class Flags : uint32 { None = 0x0, A = 0x10, B = 0xFF };", "Flags");
        Assert.Equal(new long[] { 0, 16, 255 }, e.Members.Select(m => m.Value));
    }

    [Fact]
    public void ParsesUnderlyingTypeAndMultiLineWithTrailingComma()
    {
        const string src = @"enum class Shape : uint8_t
{
    Box      = 0,
    Sphere   = 1,
    Capsule  = 2,
};";
        var e = One(src, "Shape");
        Assert.Equal(3, e.Members.Count);
        Assert.Equal("Capsule", e.Members[2].Name);
        Assert.Equal(2L, e.Members[2].Value);
    }

    [Fact]
    public void HandlesPlainEnumAndImplicitUnderlyingType()
    {
        var e = One("enum Direction { North, East, South, West };", "Direction");
        Assert.Equal(new[] { "North", "East", "South", "West" }, e.Members.Select(m => m.Name));
        Assert.Equal(new long[] { 0, 1, 2, 3 }, e.Members.Select(m => m.Value));
    }

    [Fact]
    public void StripsIntegerSuffixes()
    {
        var e = One("enum class E : uint32 { A = 1u, B = 2ul };", "E");
        Assert.Equal(new long[] { 1, 2 }, e.Members.Select(m => m.Value));
    }

    [Fact]
    public void BailsOnExpressionValues()
    {
        // A bit-shift value can't be evaluated — the enum is dropped (its fields stay integers).
        Assert.Empty(EnumScanner.Scan("enum class Flags : uint32 { A = 1 << 0, B = 1 << 1 };"));
    }

    [Fact]
    public void IgnoresForwardDeclarationWithoutBody()
    {
        Assert.Empty(EnumScanner.Scan("enum class Opaque : uint32;"));
    }

    [Fact]
    public void DoesNotMatchEnumSubstringOfIdentifier()
    {
        // `scoped_enum` / `my_enum_t` must not be parsed as an `enum` declaration.
        Assert.Empty(EnumScanner.Scan("struct my_enum_t { int scoped_enum_value; };"));
    }

    [Fact]
    public void MatchEnumStripsQualifiersAndNamespace()
    {
        var e = new EnumDecl("Bar", new List<EnumMember> { new("A", 0) });
        var enums = new Dictionary<string, EnumDecl> { ["Bar"] = e };
        var ambiguous = new HashSet<string>();

        Assert.Same(e, Program.MatchEnum("Bar", enums, ambiguous));
        Assert.Same(e, Program.MatchEnum("Foo::Bar", enums, ambiguous));
        Assert.Same(e, Program.MatchEnum("const Bar", enums, ambiguous));
        Assert.Null(Program.MatchEnum("uint32", enums, ambiguous));
    }

    [Fact]
    public void MatchEnumReturnsNullForAmbiguous()
    {
        var e = new EnumDecl("Shape", new List<EnumMember> { new("A", 0) });
        var enums = new Dictionary<string, EnumDecl> { ["Shape"] = e };
        var ambiguous = new HashSet<string> { "Shape" };
        Assert.Null(Program.MatchEnum("Shape", enums, ambiguous));
    }
}
