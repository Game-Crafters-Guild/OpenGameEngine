// The debug server is an observer, and an observer that perturbs its subject
// reports its own side effect. Two engine calls the render-stats handler is
// syntactically free to make are exactly that:
//
//   - GetOrCreateScatterPipeline loads SPIR-V, creates a Vulkan compute
//     pipeline and latches the scatter variant axes, so asking through it
//     whether a pipeline exists is what makes one exist.
//   - TryGetDrawableBindings mints a residency-gate refusal event on every
//     refusing arm, and the gate's *Total counters have no reset. One poll of
//     a view previews up to eight batch keys, so a stats read can add eight
//     refusals to the very number the mesh-pool gate's acceptance criterion
//     is read from ("this counter must read zero").
//
// Both have read-only twins (IntrospectScatter, IsEntryDrawable). The purity
// unit tests over those twins prove the twins are pure; they cannot prove the
// handler calls them, which is where the defect actually lived. This scan is
// that half: it fails if any debug-server translation unit reaches for a
// perturbing call again.
//
// The same holds one level out, for the machine the editor runs on. Tools drive
// an editor through this server while someone else works at the same desktop,
// so moving the operating-system cursor or synthesising an OS key or click lands
// under that person's hand, in whichever window has focus. Pointer and key
// requests enter the window's own input routing (Editor::InjectMouseMove,
// InjectMouseButton, InjectKey) and reach nothing outside the editor.
//
// Source-level invariant, so it reads the source tree (GE_EDITOR_SOURCE_DIR),
// the same way PanelDefaultTabIconTests locks its panel-icon defaults.

#include <gtest/gtest.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct DebugServerBannedCall
{
    const char* Symbol;
    const char* Instead;
    const char* Why;
};

// Keep this list to calls that MUTATE what the debug server observes: state a
// diagnostic reports on, or the input of the machine the editor runs on. It is
// not a general style gate. Symbols match whole identifiers only, so every
// spelling of a cursor move is its own row: the two Win32 calls, GLFW's, and
// SetCursorPosition, the name a window wrapper gives it.
const DebugServerBannedCall kDebugServerBannedCalls[] = {
    {"GetOrCreateScatterPipeline", "GPUDrawStreamBuilder::IntrospectScatter()",
     "it creates the pipeline whose existence the stats report"},
    {"TryGetDrawableBindings", "MeshGPURegistry::IsEntryDrawable()",
     "it counts a residency-gate refusal, and the gate's totals never reset"},
    {"SetCursorPos", "Editor::InjectMouseMove()",
     "it moves the operating-system cursor under the hand of whoever is using the machine"},
    {"SetPhysicalCursorPos", "Editor::InjectMouseMove()",
     "it moves the operating-system cursor under the hand of whoever is using the machine"},
    {"glfwSetCursorPos", "Editor::InjectMouseMove()",
     "it moves the operating-system cursor under the hand of whoever is using the machine"},
    {"SetCursorPosition", "Editor::InjectMouseMove()",
     "it moves the operating-system cursor under the hand of whoever is using the machine"},
    {"SendInput", "Editor::InjectMouseMove(), InjectMouseButton() or InjectKey()",
     "it synthesises operating-system input, which lands in whichever window has focus"},
    {"keybd_event", "Editor::InjectKey()",
     "it synthesises an operating-system key, which lands in whichever window has focus"},
    {"mouse_event", "Editor::InjectMouseMove() and InjectMouseButton()",
     "it synthesises operating-system mouse input, which lands under the user's hand"},
};

std::filesystem::path DebugServerSourceDir()
{
    return std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Source" / "DebugServer";
}

std::string ReadDebugServerFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Blank out comments and literal contents so a banned symbol NAMED in prose
// (the handler comments say which call they must not make, and why) or in a
// JSON key does not read as a call. Offsets are preserved so a hit still maps
// to its real line.
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

bool IsIdentifierCharacter(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

struct BannedCallSite
{
    const DebugServerBannedCall* Call;
    size_t Line;
};

// Whole identifiers only: glfwSetCursorPosCallback installs a callback and
// moves nothing, so neither SetCursorPos nor glfwSetCursorPos may match inside
// it. `code` is already stripped of comments and literals.
std::vector<BannedCallSite> FindBannedCalls(const std::string& code)
{
    std::vector<BannedCallSite> sites;
    for (const DebugServerBannedCall& banned : kDebugServerBannedCalls)
    {
        const size_t length = std::strlen(banned.Symbol);
        for (size_t at = code.find(banned.Symbol); at != std::string::npos; at = code.find(banned.Symbol, at + 1))
        {
            const size_t end = at + length;
            const bool startsIdentifier = at == 0 || !IsIdentifierCharacter(code[at - 1]);
            const bool endsIdentifier = end >= code.size() || !IsIdentifierCharacter(code[end]);
            if (startsIdentifier && endsIdentifier)
                sites.push_back({&banned, LineOfOffset(code, at)});
        }
    }
    return sites;
}

} // namespace

TEST(DebugServerReadPurity, NoHandlerReachesForAPerturbingCall)
{
    const std::filesystem::path dir = DebugServerSourceDir();
    ASSERT_TRUE(std::filesystem::is_directory(dir))
        << "debug-server sources not found at " << dir.string();

    size_t scannedFiles = 0;
    std::vector<std::string> offenders;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    {
        if (!entry.is_regular_file())
            continue;
        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".cpp" && path.extension() != ".h")
            continue;

        const std::string raw = ReadDebugServerFile(path);
        ASSERT_FALSE(raw.empty()) << "could not read " << path.string();
        ++scannedFiles;

        for (const BannedCallSite& site : FindBannedCalls(StripCommentsAndLiterals(raw)))
        {
            std::ostringstream msg;
            msg << path.filename().string() << ":" << site.Line << " calls " << site.Call->Symbol << " — "
                << site.Call->Why << ". Use " << site.Call->Instead << " instead.";
            offenders.push_back(msg.str());
        }
    }

    // A scan that matched no files would pass for the wrong reason.
    EXPECT_GT(scannedFiles, 0u) << "no debug-server sources scanned";

    std::ostringstream report;
    for (const std::string& offender : offenders)
        report << "\n  " << offender;
    EXPECT_TRUE(offenders.empty()) << "the debug server must not perturb what it observes:" << report.str();
}

// The scanner is the instrument, so prove it can see before trusting a zero.
// A stripper that blanked everything would pass this guard on any source at all,
// and a matcher that read inside longer identifiers would report a callback
// install as a cursor move.
TEST(DebugServerReadPurity, TheScannerFindsACallAndIgnoresTheSameNameInProse)
{
    const std::string sample =
        "// TryGetDrawableBindings must not be called here.\n"
        "/* nor GetOrCreateScatterPipeline in a block comment */\n"
        "const char* url = \"http://example.invalid/TryGetDrawableBindings\";\n"
        "bool ok = registry.TryGetDrawableBindings(entry, out);\n"
        "glfwSetCursorPosCallback(handle, OnCursor);\n"
        "::SetPhysicalCursorPos(x, y);\n";

    const std::string code = StripCommentsAndLiterals(sample);
    ASSERT_EQ(code.size(), sample.size()) << "the stripper must preserve offsets";

    const std::vector<BannedCallSite> sites = FindBannedCalls(code);
    ASSERT_EQ(sites.size(), 2u) << "only the calls on lines 4 and 6: the comment and the string literal are not "
                                   "calls, and installing a cursor callback moves nothing";
    EXPECT_STREQ(sites[0].Call->Symbol, "TryGetDrawableBindings");
    EXPECT_EQ(sites[0].Line, 4u);
    EXPECT_STREQ(sites[1].Call->Symbol, "SetPhysicalCursorPos");
    EXPECT_EQ(sites[1].Line, 6u);
}
