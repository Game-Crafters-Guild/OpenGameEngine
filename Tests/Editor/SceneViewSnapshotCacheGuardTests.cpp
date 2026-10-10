// A device rebuild frees every live texture's generational slot while
// TextureHandle::IsValid() keeps answering true, so a handle the scene view
// retains across frames is only safe if every read of it compares the device's
// rebuild generation first. ViewSnapshotTexture is where that comparison lives:
// it has no device-free read at all, which is what makes the check unskippable.
//
// A raw Rendering::TextureHandle member on the scene view controller opts out of
// that entirely — it is a cache with no generation to compare, and the reader
// has nothing to check even if it wants to. The bookmark-preview snapshot was
// exactly that shape, sitting beside the presentation snapshot that already had
// the guard, and nothing failed when it was added.
//
// The behavioural cases over the cache itself (ViewSnapshotTextureTests) prove
// the guard works; they cannot prove the controller routes its snapshots
// through it, which is where this defect actually lived. This scan is that
// half.
//
// COVERAGE, stated so nobody reads more into a green than it carries. The scan
// sees a fixed list of headers, and within them every syntactic form that names
// the type: by value, behind a pointer or reference, inside a template
// argument, as a public data member, and as a member of a struct declared
// inline. It CANNOT see a handle held by a type declared in some other header
// and stored here by value — `ViewPresentationSnapshot m_X` is that shape, and
// it is the sanctioned one, so an unsanctioned wrapper would read the same.
// Widening past that needs a type graph, not a text scan; the behavioural cases
// are what cover the storage type itself.
//
// Source-level invariant, so it reads the source tree (GE_EDITOR_SOURCE_DIR),
// the same way DebugServerReadPurityTests and PanelDefaultTabIconTests do.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// The headers that declare the scene view's retained state, plus the widget
// that consumes the frozen preview. ViewSnapshotTexture is the sanctioned owner
// of a raw handle and is excluded by name.
const char* const kScannedHeaders[] = {
    "Include/SceneViewController.h",
    "Include/SceneView/ViewPresentationSnapshot.h",
    "Include/UI/Controls/Widgets/CameraBookmarksWidget.h",
};

std::string ReadEditorFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Blank comment and literal contents so a member declaration quoted in prose
// does not read as a declaration. Offsets are preserved so a hit still maps to
// its real line.
std::string StripCommentsAndLiterals(const std::string& src)
{
    std::string out = src;
    enum class Scan
    {
        Code,
        LineComment,
        BlockComment,
        String,
        Char
    };
    Scan scan = Scan::Code;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const char c = out[i];
        const char next = (i + 1 < out.size()) ? out[i + 1] : '\0';
        switch (scan)
        {
        case Scan::Code:
            if (c == '/' && next == '/')
            {
                scan = Scan::LineComment;
                out[i] = out[i + 1] = ' ';
                ++i;
            }
            else if (c == '/' && next == '*')
            {
                scan = Scan::BlockComment;
                out[i] = out[i + 1] = ' ';
                ++i;
            }
            else if (c == '"')
                scan = Scan::String;
            else if (c == '\'')
                scan = Scan::Char;
            break;
        case Scan::LineComment:
            if (c == '\n')
                scan = Scan::Code;
            else
                out[i] = ' ';
            break;
        case Scan::BlockComment:
            if (c == '*' && next == '/')
            {
                scan = Scan::Code;
                out[i] = out[i + 1] = ' ';
                ++i;
            }
            else if (c != '\n')
                out[i] = ' ';
            break;
        case Scan::String:
            if (c == '\\')
            {
                out[i] = ' ';
                if (i + 1 < out.size() && out[i + 1] != '\n')
                    out[i + 1] = ' ';
                ++i;
            }
            else if (c == '"')
                scan = Scan::Code;
            else if (c != '\n')
                out[i] = ' ';
            break;
        case Scan::Char:
            if (c == '\\')
            {
                out[i] = ' ';
                if (i + 1 < out.size() && out[i + 1] != '\n')
                    out[i + 1] = ' ';
                ++i;
            }
            else if (c == '\'')
                scan = Scan::Code;
            else if (c != '\n')
                out[i] = ' ';
            break;
        }
    }
    return out;
}

size_t LineOfOffset(const std::string& src, size_t offset)
{
    size_t line = 1;
    for (size_t i = 0; i < offset && i < src.size(); ++i)
        if (src[i] == '\n')
            ++line;
    return line;
}

bool IsIdentifierChar(char c)
{
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Offsets of every occurrence that STORES a handle rather than passing one.
//
// Keying on the `m_` prefix would be the obvious rule and it is the wrong one:
// it misses a public data member, which is this repo's own convention for POD
// fields, and it misses every indirect form. So the rule is inverted — the type
// may appear only as a function's return type or as a parameter, and anything
// else that names it is storage.
std::vector<size_t> FindStoredTextureHandles(const std::string& code)
{
    constexpr const char* kType = "TextureHandle";
    const size_t typeLength = std::string::traits_type::length(kType);
    std::vector<size_t> hits;
    for (size_t at = code.find(kType); at != std::string::npos; at = code.find(kType, at + 1))
    {
        // Whole token only: `MyTextureHandle` and `TextureHandles` are other names.
        if (at > 0 && IsIdentifierChar(code[at - 1]))
            continue;
        size_t cursor = at + typeLength;
        if (cursor < code.size() && IsIdentifierChar(code[cursor]))
            continue;

        // Step over whatever sits between the type and its declarator: a
        // template's closing bracket, pointer/reference marks, whitespace.
        while (cursor < code.size() &&
               (IsSpace(code[cursor]) || code[cursor] == '>' || code[cursor] == '*' ||
                code[cursor] == '&'))
            ++cursor;

        const size_t nameStart = cursor;
        while (cursor < code.size() && IsIdentifierChar(code[cursor]))
            ++cursor;
        const bool hasDeclarator = cursor > nameStart;
        while (cursor < code.size() && IsSpace(code[cursor]))
            ++cursor;
        const char next = cursor < code.size() ? code[cursor] : '\0';

        // '(' opens a function's parameter list; ')' and ',' close a parameter.
        // Everything else — ';' '{' '=' '[', or no declarator at all (a typedef
        // or alias, which would hide the type from this scan) — is storage.
        if (hasDeclarator && (next == '(' || next == ')' || next == ','))
            continue;
        hits.push_back(at);
    }
    return hits;
}

} // namespace

TEST(SceneViewSnapshotCacheGuard, TheSceneViewStoresNoRawTextureHandle)
{
    const std::filesystem::path editorDir(GE_EDITOR_SOURCE_DIR);
    ASSERT_TRUE(std::filesystem::is_directory(editorDir))
        << "editor sources not found at " << editorDir.string();

    size_t scannedFiles = 0;
    std::vector<std::string> offenders;
    for (const char* relative : kScannedHeaders)
    {
        const std::filesystem::path path = editorDir / relative;
        const std::string raw = ReadEditorFile(path);
        ASSERT_FALSE(raw.empty()) << "could not read " << path.string();
        ++scannedFiles;

        const std::string code = StripCommentsAndLiterals(raw);
        for (const size_t at : FindStoredTextureHandles(code))
        {
            std::ostringstream msg;
            msg << relative << ":" << LineOfOffset(code, at)
                << " stores a raw TextureHandle. A device rebuild frees its slot while "
                   "IsValid() still answers true; hold it in ViewSnapshotTexture (via "
                   "ViewPresentationSnapshot) so the read compares the rebuild generation.";
            offenders.push_back(msg.str());
        }
    }

    // A scan that matched no files would pass for the wrong reason.
    ASSERT_EQ(scannedFiles, std::size(kScannedHeaders)) << "not every scanned header was read";

    std::ostringstream report;
    for (const std::string& offender : offenders)
        report << "\n  " << offender;
    EXPECT_TRUE(offenders.empty())
        << "scene view snapshots must be device-scoped:" << report.str();
}

// The scanner is the instrument, so prove it can see before trusting a zero.
// A matcher that found nothing anywhere would pass the guard above on any
// source at all — and one keyed to `m_` passes every storing form below except
// the first, which is how those forms got in here.
TEST(SceneViewSnapshotCacheGuard, TheScannerSeesEveryStoringFormAndNoPassingOne)
{
    // Lines 1-3 name the type without declaring anything; 4-6 pass it; 7-11
    // store it. Line 10 is the repo's own convention for public POD fields, so
    // a rule keyed to the m_ prefix reads it as clean.
    const std::string sample =
        "// Rendering::TextureHandle m_InALineComment is the shape being banned.\n"   // 1
        "/* nor TextureHandle m_InABlockComment */\n"                                 // 2
        "const char* doc = \"TextureHandle m_InAStringLiteral\";\n"                   // 3
        "Rendering::TextureHandle GetPreviewSnapshotTexture(const IDevice&) const;\n" // 4
        "void Bind(Rendering::TextureHandle snapshot);\n"                             // 5
        "void Bind2(Rendering::TextureHandle snapshot, uint32_t w);\n"                // 6
        "Rendering::TextureHandle m_PreviewSnapshotTex{};\n"                          // 7
        "std::vector<Rendering::TextureHandle> m_Handles;\n"                          // 8
        "Rendering::TextureHandle* m_HandlePtr = nullptr;\n"                          // 9
        "Rendering::TextureHandle PublicPod{};\n"                                     // 10
        "struct Wrap { Rendering::TextureHandle Held; };\n";                          // 11

    const std::string code = StripCommentsAndLiterals(sample);
    ASSERT_EQ(code.size(), sample.size()) << "the stripper must preserve offsets";

    std::vector<size_t> lines;
    for (const size_t at : FindStoredTextureHandles(code))
        lines.push_back(LineOfOffset(code, at));

    const std::vector<size_t> expected{7u, 8u, 9u, 10u, 11u};
    EXPECT_EQ(lines, expected)
        << "every storing form must be seen, and no comment, literal, return type or "
           "parameter may read as one";
}
