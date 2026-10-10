using System.Linq;
using GameEngine.ComponentScanner;
using Xunit;

namespace GameEngine.ComponentScanner.Tests;

public class CommentStripperTests
{
    [Fact]
    public void RemovesLineCommentButKeepsCode()
    {
        string outp = CommentStripper.Strip("int x; // comment with ; and }\nint y;");
        Assert.DoesNotContain("comment", outp);
        Assert.Contains("int x;", outp);
        Assert.Contains("int y;", outp);
    }

    [Fact]
    public void RemovesBlockCommentIncludingItsBracesAndSemicolons()
    {
        string outp = CommentStripper.Strip("int /* ; } */ x;");
        Assert.Equal(1, outp.Count(c => c == ';')); // only the real terminator survives
        Assert.DoesNotContain("}", outp);
        Assert.Contains("x", outp);
    }

    [Fact]
    public void RemovesStringLiteralContentsSoBracesCannotConfuseTheParser()
    {
        string outp = CommentStripper.Strip("const char* s = \"a;b}c{\";");
        Assert.DoesNotContain("a;b", outp);
        Assert.DoesNotContain("}", outp);
    }

    [Fact]
    public void PreservesNewlineCountForStableLineNumbers()
    {
        string outp = CommentStripper.Strip("a // x\nb // y\nc");
        Assert.Equal(3, outp.Replace("\r\n", "\n").Split('\n').Length);
    }
}
