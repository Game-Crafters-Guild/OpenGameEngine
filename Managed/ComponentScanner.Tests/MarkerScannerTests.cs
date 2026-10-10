using GameEngine.ComponentScanner;
using Xunit;

namespace GameEngine.ComponentScanner.Tests;

public class MarkerScannerTests
{
    private static string[] Lines(string s) => s.Replace("\r\n", "\n").Split('\n');

    [Fact]
    public void DetectsEachMarkerInLineComments()
    {
        var m = MarkerScanner.Scan(Lines("a // @ge-no-add\nb // @ge-readonly\nc // @ge-hidden\nd"));
        Assert.Contains(1, m.NoAdd);
        Assert.Contains(2, m.ReadOnly);
        Assert.Contains(3, m.Hidden);
        Assert.DoesNotContain(4, m.NoAdd);
    }

    [Fact]
    public void DetectsEditorOnlyMarkerOnlyInALineComment()
    {
        var m = MarkerScanner.Scan(Lines("// @ge-editor-only  stripped from exports\nstruct Measure {};\nconst char* s = \"@ge-editor-only\";"));
        Assert.Contains(1, m.EditorOnly);
        Assert.DoesNotContain(3, m.EditorOnly);
        Assert.Empty(m.NoAdd);
    }

    [Fact]
    public void DetectsMarkerInTrailingComment()
    {
        var m = MarkerScanner.Scan(Lines("float Radius = 1.0f;  // @ge-readonly runtime handle"));
        Assert.Contains(1, m.ReadOnly);
    }

    [Fact]
    public void IgnoresMarkerTextOutsideALineComment()
    {
        // No "//" on the line -> not a marker (e.g. the token appears in code or a string).
        var m = MarkerScanner.Scan(Lines("const char* s = \"@ge-hidden\";\nint x; @ge-readonly"));
        Assert.Empty(m.Hidden);
        Assert.Empty(m.ReadOnly);
    }

    [Fact]
    public void RecordsMultipleDistinctMarkersOnOneLine()
    {
        var m = MarkerScanner.Scan(Lines("int x;  // @ge-readonly @ge-hidden"));
        Assert.Contains(1, m.ReadOnly);
        Assert.Contains(1, m.Hidden);
    }

    [Theory]
    [InlineData("float Interval;  // @ge-range 0", 0.0, null)]
    [InlineData("float Interval;  // @ge-range 0 seconds, open maximum", 0.0, null)]
    [InlineData("float Interval;  // @ge-range: 0 60", 0.0, 60.0)]
    [InlineData("float Bias = 0.5f;  // @ge-range -1.5 1.5 signed", -1.5, 1.5)]
    public void CapturesRangeBounds(string line, double min, double? max)
    {
        var m = MarkerScanner.Scan(Lines(line));
        Assert.True(m.Ranges.TryGetValue(1, out FieldRange? range));
        Assert.Equal(min, range!.Min);
        Assert.Equal(max, range.Max);
    }

    [Theory]
    [InlineData("float Interval;  // @ge-range")]
    [InlineData("float Interval;  // @ge-range abc")]
    [InlineData("float Interval;  // @ge-range 1 0")]
    [InlineData("float Interval;  // @ge-range NaN 1")]
    public void RejectsMalformedRange(string line)
    {
        var m = MarkerScanner.Scan(Lines(line));
        Assert.Empty(m.Ranges);
    }

    [Theory]
    [InlineData("float Strength;  // @ge-tooltip Strength of the reflection.", "Strength of the reflection.")]
    [InlineData("float Strength;  // @ge-tooltip: Strength of the reflection.", "Strength of the reflection.")]
    [InlineData("float Strength;  // @ge-tooltip = Strength of the reflection.", "Strength of the reflection.")]
    [InlineData("float Strength;  // @ge-tooltip \"Strength of the \\\"reflection\\\".\"", "Strength of the \"reflection\".")]
    public void CapturesTooltipText(string line, string expected)
    {
        var m = MarkerScanner.Scan(Lines(line));
        Assert.True(m.Tooltips.TryGetValue(1, out string? tooltip));
        Assert.Equal(expected, tooltip);
    }

    [Theory]
    [InlineData("// [DoNotSerialize]")]      // canonical
    [InlineData("// [NonSerializable]")]     // alias
    [InlineData("// [DontSerialize]")]       // alias
    [InlineData("// @DoNotSerialize")]       // @ form
    [InlineData("// @NonSerializable")]      // @ alias
    [InlineData("struct Foo {  // [DoNotSerialize] runtime GPU handle")] // trailing, with prose
    public void DetectsDoNotSerializeMarkerAndAliases(string line)
    {
        var m = MarkerScanner.Scan(Lines(line));
        Assert.Contains(1, m.DoNotSerialize);
    }

    [Theory]
    [InlineData("// [donotserialize]")]
    [InlineData("// [DONOTSERIALIZE]")]
    [InlineData("// [DoNotSerialize]")]
    public void DoNotSerializeIsCaseInsensitive(string line)
    {
        var m = MarkerScanner.Scan(Lines(line));
        Assert.Contains(1, m.DoNotSerialize);
    }

    [Fact]
    public void DoNotSerializeIgnoredOutsideALineComment()
    {
        // The token in code or a string literal (no "//") is not a marker.
        var m = MarkerScanner.Scan(Lines("const char* s = \"[DoNotSerialize]\";\nint DoNotSerialize;"));
        Assert.Empty(m.DoNotSerialize);
    }
}
