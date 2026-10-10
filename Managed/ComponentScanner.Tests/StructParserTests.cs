using GameEngine.ComponentScanner;
using Xunit;

namespace GameEngine.ComponentScanner.Tests;

// StructParser operates on comment-stripped source; these fixtures are already comment-free,
// so the parser's line numbers line up with the fixture's own lines.
public class StructParserTests
{
    private static ParsedStruct ParseOne(string clean, string name) =>
        StructParser.Parse(clean).Single(s => s.Name == name);

    private static string[] Names(ParsedStruct s) => s.Fields.Select(f => f.Name).ToArray();

    [Fact]
    public void ExtractsScalarAndArrayFieldsInDeclarationOrder()
    {
        const string src = @"namespace GameEngine::Components {
struct Probe {
    float X;
    int32 Count;
    char Label[16];
};
}";
        var s = ParseOne(src, "Probe");
        Assert.Equal("GameEngine::Components", s.Namespace);
        Assert.Equal(new[] { "X", "Count", "Label" }, Names(s));
    }

    [Fact]
    public void RecoversFieldsWithFunctionStyleCastOrCallInitializer()
    {
        // Regression guard for the '(' -after- top-level '=' handling (HasTopLevelAssign):
        // a '(' inside an initializer is NOT a function parameter list, so the field survives.
        const string src = @"namespace GameEngine::Components {
struct E {
    uint32 Mode = static_cast<uint32>(SomeEnum::Value);
    uint32 Plain = 3u;
    float Made = MakeThing(1, 2);
};
}";
        var s = ParseOne(src, "E");
        Assert.Equal(new[] { "Mode", "Plain", "Made" }, Names(s));
    }

    [Fact]
    public void DoesNotMisparseAssignmentOperatorAsField()
    {
        // operator= ends in '=' before '(' but is a function, not an initializer.
        const string src = @"namespace GameEngine::Components {
struct WithOp {
    float X;
    WithOp& operator=(const WithOp& other);
    bool operator==(const WithOp& other) const;
};
}";
        var s = ParseOne(src, "WithOp");
        Assert.Equal(new[] { "X" }, Names(s));
        Assert.False(s.HasVirtual);
        Assert.Null(s.UnexpandedMacro);
    }

    [Fact]
    public void ExtractsCommaSeparatedDeclarators()
    {
        const string src = @"namespace GameEngine::Components {
struct C {
    int32 A, B, Cc;
};
}";
        var s = ParseOne(src, "C");
        Assert.Equal(new[] { "A", "B", "Cc" }, Names(s));
    }

    [Fact]
    public void FlagsUnexpandedFieldInjectionMacro()
    {
        // A bare ALL-CAPS macro token injects fields the scanner can't see -> recorded so the
        // caller skips the struct (mirrors the terrain modifiers' GE_TERRAIN_MODIFIER_COMMON_FIELDS).
        const string src = @"namespace GameEngine::Components {
struct M {
    GE_SOME_COMMON_FIELDS
    float Extra;
};
}";
        var s = ParseOne(src, "M");
        Assert.Equal("GE_SOME_COMMON_FIELDS", s.UnexpandedMacro);
    }

    [Fact]
    public void DetectsVirtualMember()
    {
        const string src = @"namespace GameEngine::Components {
struct V {
    float X;
    virtual void Tick();
};
}";
        var s = ParseOne(src, "V");
        Assert.True(s.HasVirtual);
    }

    [Fact]
    public void SkipsMethodsAndStaticsButKeepsDataMembers()
    {
        const string src = @"namespace GameEngine::Components {
struct Mixed {
    static constexpr int Cap = 8;
    int32 Count;
    void Reset() { Count = 0; }
    float Values[4];
};
}";
        var s = ParseOne(src, "Mixed");
        Assert.Equal(new[] { "Count", "Values" }, Names(s));
    }

    [Fact]
    public void RecordsFieldDeclarationLineNumbers()
    {
        // Lines: 1 namespace, 2 struct, 3 First, 4 Second, 5 '}'.
        const string src = "namespace GameEngine::Components {\nstruct L {\n    float First;\n    int32 Second;\n};\n}";
        var s = ParseOne(src, "L");
        Assert.Equal(3, s.Fields.Single(f => f.Name == "First").DeclLine);
        Assert.Equal(4, s.Fields.Single(f => f.Name == "Second").DeclLine);
    }

    [Fact]
    public void SkipsTemplateStructDefinitions()
    {
        // A `template<...> struct` is a template, not a component — registering it would emit
        // GE_REGISTER_COMPONENT on an unspecialized template (won't compile). A concrete sibling
        // declared right after it must still parse.
        const string src = @"namespace GameEngine::Components {
template<AssetType kType = AssetType::Unknown>
struct AssetRef {
    uint8 Bytes[16];
};
struct Concrete {
    float X;
};
}";
        var all = StructParser.Parse(src);
        Assert.DoesNotContain(all, s => s.Name == "AssetRef");
        Assert.Contains(all, s => s.Name == "Concrete");
    }

    [Fact]
    public void RecordsMultiLineInitializerEndLine()
    {
        // A brace initializer spanning lines: DeclLine at the head, EndLine at the ';'.
        const string src = "namespace GameEngine::Components {\nstruct A {\n    float Axis[3] = {0.0f,\n                    1.0f,\n                    0.0f};\n};\n}";
        var s = ParseOne(src, "A");
        var axis = s.Fields.Single(f => f.Name == "Axis");
        Assert.Equal(3, axis.DeclLine);
        Assert.Equal(5, axis.EndLine);
    }
}
