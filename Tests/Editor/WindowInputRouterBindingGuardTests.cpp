// One router per OS window, and every input event enters through it.
//
// A window whose platform handlers are bound by hand gets whatever subset of the
// chain its author remembered. The colour picker dialog was that shape: it bound
// six handlers, bound no focus handler at all, and therefore never reset the
// modifiers it was holding when the window lost focus — a Shift left down across
// an app switch stays down for the next drag. Nothing failed; a leg was simply
// absent, and no test could see it, because the defect is what the file does
// *not* contain.
//
// This scan is that half. WindowInputRouter::BindBasicHandlers is the one place
// production code binds a window's key, char, mouse, scroll, cursor-enter and
// focus handlers; every other binding in Apps/ and Engine/ is a bypass. Teardown
// clears — `SetKeyHandler({})` — are not routing and are allowed by form.
//
// COVERAGE, stated so nobody reads more into a green than it carries: this is a
// text scan over source files, so it sees the direct call form on a window
// object. It cannot see a binding made through an alias whose name it does not
// know, nor one assembled at runtime. Examples/ is out of scope: a standalone
// demo with no UIManager and no InputSystem has no chain to enter, and
// PhysicsFallingSpheresDemo says so in its own comment.
//
// What the scan reads is comment- and literal-stripped text, and the stripper is
// a lexer only that far: it does not know the raw string form, so a raw string
// holding a quote character ends early and the text after it is read as code.
// That miscount is loud — it can name a bypass that is not one — rather than the
// silent kind, where a file goes unread from some offset to its end and a binding
// after that point is simply never seen. Two rows pin the two openers that decide
// which of those happens.
//
// Source-level invariant, so it reads the source tree (GE_REPO_SOURCE_DIR), the
// same way SceneViewSnapshotCacheGuardTests and DebugServerReadPurityTests do.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
// The production trees that create OS windows. Tests drive routing directly and
// are excluded by path below.
const char* const kScannedRoots[] = {"Apps", "Engine"};

// The one file allowed to bind them: the router itself.
const char* const kRouterSource = "Engine/Source/Core/WindowInputRouter.cpp";

const char* const kHandlerSetters[] = {
    "SetKeyHandler", "SetCharHandler", "SetMouseButtonHandler", "SetMouseMoveHandler", "SetScrollHandler",
    "SetFocusHandler", "SetCursorEnterHandler",
};

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Blank comment bodies and literal contents so prose naming a setter does not
// read as a call and a path like "/*.cpp" does not open a comment. Newlines and
// offsets survive, so a hit still maps to its real line.
std::string StripComments(const std::string& src)
{
    std::string out = src;
    const size_t size = src.size();

    auto blank = [&out](size_t from, size_t to)
    {
        for (size_t k = from; k <= to && k < out.size(); ++k)
            if (out[k] != '\n')
                out[k] = ' ';
    };

    size_t i = 0;
    while (i + 1 < size)
    {
        if (src[i] == '/' && src[i + 1] == '/')
        {
            size_t end = i;
            while (end < size && src[end] != '\n')
                ++end;
            blank(i, end - 1);
            i = end;
        }
        else if (src[i] == '/' && src[i + 1] == '*')
        {
            // The earliest `/` that can close the comment is three bytes past the
            // opener, so `/*/` does not close on its own opener. An unterminated
            // comment runs to end of file, which is what the compiler does too.
            size_t end = i + 3;
            while (end < size && !(src[end - 1] == '*' && src[end] == '/'))
                ++end;
            const size_t last = (end < size) ? end : size - 1;
            blank(i, last);
            i = last + 1;
        }
        else if (src[i] == '"' || src[i] == '\'')
        {
            // A literal's contents are not code. The quotes themselves stay so the
            // text still reads as a literal rather than as an identifier.
            const char quote = src[i];
            size_t end = i + 1;
            while (end < size && src[end] != quote && src[end] != '\n')
            {
                if (src[end] == '\\' && end + 1 < size)
                    ++end;
                ++end;
            }
            if (end > i + 1)
                blank(i + 1, end - 1);
            i = end + 1;
        }
        else
        {
            ++i;
        }
    }
    return out;
}

size_t LineOf(const std::string& text, size_t offset)
{
    return 1 + static_cast<size_t>(std::count(text.begin(), text.begin() + static_cast<long long>(offset), '\n'));
}

// `SetKeyHandler({})` and friends unbind a handler at teardown. They install no
// routing, so they are not bypasses.
bool IsClear(const std::string& text, size_t afterOpenParen)
{
    size_t i = afterOpenParen;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
    return i + 1 < text.size() && text[i] == '{' && text[i + 1] == '}';
}

bool IsSourceFile(const std::filesystem::path& path)
{
    const std::string ext = path.extension().string();
    return ext == ".cpp" || ext == ".h" || ext == ".mm" || ext == ".m";
}

std::string ToPortablePath(const std::filesystem::path& path)
{
    std::string s = path.generic_string();
    return s;
}

struct ScanResult
{
    std::vector<std::string> bypasses;
    size_t routerBinds = 0;
    size_t filesScanned = 0;
};

void ScanSource(const std::string& raw, const std::string& relative, ScanResult& result)
{
    ++result.filesScanned;
    const std::string text = StripComments(raw);
    const bool isRouter = (relative == kRouterSource);

    for (const char* setter : kHandlerSetters)
    {
        const std::string needle = setter;
        size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos)
        {
            const size_t nameStart = pos;
            pos += needle.size();

            // Member call syntax only: a declaration or definition of the setter
            // itself has no object in front of it.
            const bool calledOnObject =
                nameStart >= 2 && (text[nameStart - 1] == '.' ||
                                   (text[nameStart - 1] == '>' && text[nameStart - 2] == '-'));
            if (!calledOnObject)
                continue;

            size_t open = pos;
            while (open < text.size() && std::isspace(static_cast<unsigned char>(text[open])))
                ++open;
            if (open >= text.size() || text[open] != '(')
                continue;

            if (isRouter)
            {
                ++result.routerBinds;
                continue;
            }
            if (IsClear(text, open + 1))
                continue;
            result.bypasses.push_back(relative + ":" + std::to_string(LineOf(text, nameStart)) + " (" + needle + ")");
        }
    }
}

void ScanFile(const std::filesystem::path& path, const std::string& relative, ScanResult& result)
{
    const std::string raw = ReadFile(path);
    if (raw.empty())
        return;
    ScanSource(raw, relative, result);
}
} // namespace

// The terminator the whole scan rests on: a block comment ends where its `*/` is,
// and a binding written after one is still a binding. Its body is hidden, and the
// scan resumes at the same file rather than going blind to end of file.
TEST(WindowInputRouterBindingGuardTests, ABlockCommentHidesOnlyItsOwnBody)
{
    const std::string source = "/* window->SetKeyHandler(handler) in prose is not a binding. */\n"
                               "void Bind(Platform::Window* window)\n"
                               "{\n"
                               "    window->SetKeyHandler([](int, int, int) {});\n"
                               "}\n"
                               "/* A second block, so a hit after one proves the scan resumed. */\n"
                               "void Unbind(Platform::Window* window)\n"
                               "{\n"
                               "    window->SetCharHandler({});\n"
                               "}\n";

    ScanResult result;
    ScanSource(source, "Apps/Editor/Source/Synthetic.cpp", result);

    EXPECT_EQ(result.filesScanned, 1u);
    ASSERT_EQ(result.bypasses.size(), 1u)
        << "expected exactly the hand binding on line 4: prose is stripped, a teardown clear is allowed";
    EXPECT_EQ(result.bypasses[0], "Apps/Editor/Source/Synthetic.cpp:4 (SetKeyHandler)");
}

// The other opener. A glob or a path in a string literal carries `/*`, and the
// tree holds such strings today; reading one as a comment would take the rest of
// that file out of the scan without saying so.
TEST(WindowInputRouterBindingGuardTests, AStringLiteralDoesNotOpenAComment)
{
    const std::string source = "void Glob()\n"
                               "{\n"
                               "    Collect(\"${DIR}/*.cpp\");\n"
                               "}\n"
                               "void Bind(Platform::Window* window)\n"
                               "{\n"
                               "    window->SetFocusHandler([](bool) {});\n"
                               "}\n";

    ScanResult result;
    ScanSource(source, "Apps/Editor/Source/Synthetic.cpp", result);

    ASSERT_EQ(result.bypasses.size(), 1u) << "the file must stay readable past a string holding a comment opener";
    EXPECT_EQ(result.bypasses[0], "Apps/Editor/Source/Synthetic.cpp:7 (SetFocusHandler)");
}

TEST(WindowInputRouterBindingGuardTests, OnlyTheRouterBindsAWindowsInputHandlers)
{
    const std::filesystem::path repoRoot(GE_REPO_SOURCE_DIR);
    ASSERT_TRUE(std::filesystem::is_directory(repoRoot)) << "repo sources not found at " << repoRoot.string();

    ScanResult result;
    for (const char* root : kScannedRoots)
    {
        const std::filesystem::path rootPath = repoRoot / root;
        ASSERT_TRUE(std::filesystem::is_directory(rootPath)) << "missing scan root " << rootPath.string();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(rootPath))
        {
            if (!entry.is_regular_file() || !IsSourceFile(entry.path()))
                continue;
            const std::string relative = ToPortablePath(std::filesystem::relative(entry.path(), repoRoot));
            if (relative.find("/Tests/") != std::string::npos)
                continue;
            ScanFile(entry.path(), relative, result);
        }
    }

    // Positive control: a scan that reached the wrong tree, or a pattern that no
    // longer matches the call form, would report no bypasses for the wrong
    // reason. The router binds every handler in this list, so finding its own
    // bindings is what makes an empty bypass list mean something.
    EXPECT_GE(result.filesScanned, 500u) << "scan covered implausibly few files";
    EXPECT_GE(result.routerBinds, std::size(kHandlerSetters))
        << "the router's own bindings were not found — the scan is not reading " << kRouterSource;

    // What the green covered, so the reach of a pass is readable rather than
    // assumed. Surfaces in the XML report alongside the result.
    RecordProperty("filesScanned", static_cast<int>(result.filesScanned));
    RecordProperty("routerBinds", static_cast<int>(result.routerBinds));
    RecordProperty("bypasses", static_cast<int>(result.bypasses.size()));

    std::string report;
    for (const std::string& bypass : result.bypasses)
        report += "\n  " + bypass;
    EXPECT_TRUE(result.bypasses.empty())
        << "these bind a window's input handlers outside WindowInputRouter::BindBasicHandlers, so the window gets "
           "only the legs its author remembered:"
        << report;
}
