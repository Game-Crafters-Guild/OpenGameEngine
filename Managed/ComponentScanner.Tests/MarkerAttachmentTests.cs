using GameEngine.ComponentScanner;
using Xunit;

namespace GameEngine.ComponentScanner.Tests;

public class MarkerAttachmentTests
{
    private static string[] Lines(string s) => s.Replace("\r\n", "\n").Split('\n');

    [Fact]
    public void FieldMarkerAttachesOnTheFieldsOwnLine()
    {
        var lines = Lines("struct S {\n    int A;  // @ge-readonly\n    int B;\n};");
        Assert.True(MarkerAttachment.FieldMarkerAttached(lines, declLine: 2, endLine: 2, new HashSet<int> { 2 }));
    }

    [Fact]
    public void FieldMarkerAttachesOnPrecedingCommentLine()
    {
        var lines = Lines("struct S {\n    // @ge-hidden\n    int Secret;\n};");
        Assert.True(MarkerAttachment.FieldMarkerAttached(lines, declLine: 3, endLine: 3, new HashSet<int> { 2 }));
    }

    [Fact]
    public void FieldValueMarkerUsesSameAttachmentRules()
    {
        var lines = Lines("struct S {\n    // @ge-tooltip Visible strength.\n    float Strength;\n    float Other;\n};");
        var values = new Dictionary<int, string> { [2] = "Visible strength." };

        Assert.Equal("Visible strength.", MarkerAttachment.FieldValueMarkerAttached(lines, 3, 3, values));
        Assert.Null(MarkerAttachment.FieldValueMarkerAttached(lines, 4, 4, values));
    }

    [Fact]
    public void FieldMarkerDoesNotLeakToTheNextField()
    {
        // A trailing "// @ge-readonly" on field A (line 2) must NOT attach to field B (line 3):
        // field A's line is code, so it ends the preceding-comment run for B.
        var lines = Lines("struct S {\n    int A;  // @ge-readonly\n    int B;\n};");
        Assert.False(MarkerAttachment.FieldMarkerAttached(lines, declLine: 3, endLine: 3, new HashSet<int> { 2 }));
    }

    [Fact]
    public void FieldMarkerAttachesAnywhereInAMultiLineDeclarationRange()
    {
        // The marker sits on the terminating ';' line of a multi-line initializer.
        var lines = Lines("struct S {\n    float Axis[3] = {0,\n                    1,\n                    0};  // @ge-hidden\n};");
        Assert.True(MarkerAttachment.FieldMarkerAttached(lines, declLine: 2, endLine: 4, new HashSet<int> { 4 }));
    }

    [Fact]
    public void StructMarkerAttachesOnDeclarationAndPrecedingRun()
    {
        var onDecl = Lines("struct Helper {  // @ge-no-add\n    int X;\n};");
        Assert.True(MarkerAttachment.StructMarkerAttached(onDecl, declLine: 1, new HashSet<int> { 1 }));

        var onPreceding = Lines("// @ge-no-add\nstruct Helper {\n    int X;\n};");
        Assert.True(MarkerAttachment.StructMarkerAttached(onPreceding, declLine: 2, new HashSet<int> { 1 }));
    }

    [Fact]
    public void StructMarkerDoesNotLeakPastAnInterveningCodeLine()
    {
        // Marker on line 1, but an unrelated declaration on line 2 breaks the run before
        // the target struct on line 3.
        var lines = Lines("// @ge-no-add\nstruct Other {};\nstruct Target {\n    int X;\n};");
        Assert.False(MarkerAttachment.StructMarkerAttached(lines, declLine: 3, new HashSet<int> { 1 }));
    }

    [Fact]
    public void NoMarkersMeansNotAttached()
    {
        var lines = Lines("struct S {\n    int A;\n};");
        Assert.False(MarkerAttachment.FieldMarkerAttached(lines, 2, 2, new HashSet<int>()));
        Assert.False(MarkerAttachment.StructMarkerAttached(lines, 1, new HashSet<int>()));
    }
}
