using GameEngine.ComponentScanner;
using Xunit;

namespace GameEngine.ComponentScanner.Tests;

// Covers the inheritance-based component detection (--detect-base ComponentBase), used
// to auto-register user components declared as `struct X : ECS::ComponentBase { ... }`
// with no macro. Detection = the struct's base-clause has a base whose simple name matches.
public class BaseDetectionTests
{
    [Theory]
    [InlineData("public ECS::ComponentBase", true)]
    [InlineData("ECS::ComponentBase", true)]
    [InlineData("GameEngine::ECS::ComponentBase", true)]
    [InlineData("ComponentBase", true)]
    [InlineData("public virtual ECS::ComponentBase", true)]
    [InlineData("public A, public ECS::ComponentBase", true)] // second of two bases
    [InlineData("", false)]
    [InlineData("public SomethingElse", false)]
    [InlineData("public ECS::Component", false)] // the concept name must NOT match the tag
    [InlineData("public std::enable_shared_from_this<Foo>", false)]
    public void InheritsBase_matchesSimpleName(string bases, bool expected)
    {
        Assert.Equal(expected, Program.InheritsBase(bases, "ComponentBase"));
    }

    [Fact]
    public void StructParser_capturesBaseClause()
    {
        var structs = StructParser.Parse("struct Health : public ECS::ComponentBase { float Current; int Lives; };");
        var s = Assert.Single(structs);
        Assert.Equal("Health", s.Name);
        Assert.Contains("ComponentBase", s.Bases);
        Assert.Equal(2, s.Fields.Count); // Current, Lives
    }

    [Fact]
    public void StructParser_plainStructHasNoBases()
    {
        var structs = StructParser.Parse("struct Plain { float X; };");
        var s = Assert.Single(structs);
        Assert.Equal("", s.Bases);
    }

    // InheritingStructNames backs the .cpp diagnostic: a macro-free component authored in a
    // .cpp (not a header) can't be reflected, so the scanner names it for a build warning.
    [Fact]
    public void InheritingStructNames_findsInheritorsForWarning()
    {
        const string src =
            "namespace game { struct Speed : ECS::ComponentBase { float V; }; }\n" +
            "struct Plain { int X; };\n" +
            "struct Mana : public ECS::ComponentBase { float M; };";
        var names = Program.InheritingStructNames(src, "ComponentBase");
        Assert.Equal(new[] { "Speed", "Mana" }, names);
    }

    [Fact]
    public void InheritingStructNames_emptyWhenNoneInherit()
    {
        var names = Program.InheritingStructNames("struct A { int X; }; struct B : public Other { int Y; };", "ComponentBase");
        Assert.Empty(names);
    }
}
