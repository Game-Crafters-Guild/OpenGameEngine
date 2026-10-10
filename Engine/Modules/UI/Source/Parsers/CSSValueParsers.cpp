// CSS value/keyword parsers and declaration expansion. Split out of
// CSSParser.cpp (P4f); cold parse-time code, not on the per-frame cascade path.

#include "UI/Parsers/CSSParser.h"
#include "CSSParserDetail.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"

#include "Types/StringUtils.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace GameEngine
{
namespace UIParsing
{
using namespace CSSDetail;

namespace
{

static inline std::string RemoveWhitespace(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
            out.push_back(c);
    }
    return out;
}

// Every CSS colour form the engine reads: hex, 0xAARRGGBB, named colours, rgb()/rgba(),
// hsl()/hsla(), decimal ARGB. Null when the text is none of them.
std::optional<uint32_t> TryParseColorValue(const std::string& v)
{
    auto hex = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f')
            return 10 + (c - 'a');
        return 0;
    };

    auto clamp255 = [](int x) -> int
    { return std::max(0, std::min(255, x)); };
    auto toARGB = [&](int r, int g, int b, int a) -> std::optional<uint32_t>
    {
        r = clamp255(r);
        g = clamp255(g);
        b = clamp255(b);
        a = clamp255(a);
        return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    };

    std::string s = Trim(v);
    std::string sl = ToLowerAscii(s);

    // Full CSS Level 4 named colors (ARGB) + transparent.
    // Thread-safe: C++11 guarantees static local initialization is done exactly once.
    static const std::unordered_map<std::string, uint32_t> kNamedColors = {
        {"black",0xFF000000u},{"silver",0xFFC0C0C0u},{"gray",0xFF808080u},{"grey",0xFF808080u},{"white",0xFFFFFFFFu},
        {"maroon",0xFF800000u},{"red",0xFFFF0000u},{"purple",0xFF800080u},{"fuchsia",0xFFFF00FFu},
        {"green",0xFF008000u},{"lime",0xFF00FF00u},{"olive",0xFF808000u},{"yellow",0xFFFFFF00u},
        {"navy",0xFF000080u},{"blue",0xFF0000FFu},{"teal",0xFF008080u},{"aqua",0xFF00FFFFu},
        {"orange",0xFFFFA500u},{"aliceblue",0xFFF0F8FFu},{"antiquewhite",0xFFFAEBD7u},
        {"aquamarine",0xFF7FFFD4u},{"azure",0xFFF0FFFFu},{"beige",0xFFF5F5DCu},{"bisque",0xFFFFE4C4u},
        {"blanchedalmond",0xFFFFEBCDu},{"blueviolet",0xFF8A2BE2u},{"brown",0xFFA52A2Au},
        {"burlywood",0xFFDEB887u},{"cadetblue",0xFF5F9EA0u},{"chartreuse",0xFF7FFF00u},
        {"chocolate",0xFFD2691Eu},{"coral",0xFFFF7F50u},{"cornflowerblue",0xFF6495EDu},
        {"cornsilk",0xFFFFF8DCu},{"crimson",0xFFDC143Cu},{"cyan",0xFF00FFFFu},
        {"darkblue",0xFF00008Bu},{"darkcyan",0xFF008B8Bu},{"darkgoldenrod",0xFFB8860Bu},
        {"darkgray",0xFFA9A9A9u},{"darkgreen",0xFF006400u},{"darkgrey",0xFFA9A9A9u},
        {"darkkhaki",0xFFBDB76Bu},{"darkmagenta",0xFF8B008Bu},{"darkolivegreen",0xFF556B2Fu},
        {"darkorange",0xFFFF8C00u},{"darkorchid",0xFF9932CCu},{"darkred",0xFF8B0000u},
        {"darksalmon",0xFFE9967Au},{"darkseagreen",0xFF8FBC8Fu},{"darkslateblue",0xFF483D8Bu},
        {"darkslategray",0xFF2F4F4Fu},{"darkslategrey",0xFF2F4F4Fu},{"darkturquoise",0xFF00CED1u},
        {"darkviolet",0xFF9400D3u},{"deeppink",0xFFFF1493u},{"deepskyblue",0xFF00BFFFu},
        {"dimgray",0xFF696969u},{"dimgrey",0xFF696969u},{"dodgerblue",0xFF1E90FFu},
        {"firebrick",0xFFB22222u},{"floralwhite",0xFFFFFAF0u},{"forestgreen",0xFF228B22u},
        {"gainsboro",0xFFDCDCDCu},{"ghostwhite",0xFFF8F8FFu},{"gold",0xFFFFD700u},
        {"goldenrod",0xFFDAA520u},{"greenyellow",0xFFADFF2Fu},{"honeydew",0xFFF0FFF0u},
        {"hotpink",0xFFFF69B4u},{"indianred",0xFFCD5C5Cu},{"indigo",0xFF4B0082u},
        {"ivory",0xFFFFFFF0u},{"khaki",0xFFF0E68Cu},{"lavender",0xFFE6E6FAu},
        {"lavenderblush",0xFFFFF0F5u},{"lawngreen",0xFF7CFC00u},{"lemonchiffon",0xFFFFFACDu},
        {"lightblue",0xFFADD8E6u},{"lightcoral",0xFFF08080u},{"lightcyan",0xFFE0FFFFu},
        {"lightgoldenrodyellow",0xFFFAFAD2u},{"lightgray",0xFFD3D3D3u},{"lightgreen",0xFF90EE90u},
        {"lightgrey",0xFFD3D3D3u},{"lightpink",0xFFFFB6C1u},{"lightsalmon",0xFFFFA07Au},
        {"lightseagreen",0xFF20B2AAu},{"lightskyblue",0xFF87CEFAu},{"lightslategray",0xFF778899u},
        {"lightslategrey",0xFF778899u},{"lightsteelblue",0xFFB0C4DEu},{"lightyellow",0xFFFFFFE0u},
        {"limegreen",0xFF32CD32u},{"linen",0xFFFAF0E6u},{"magenta",0xFFFF00FFu},
        {"mediumaquamarine",0xFF66CDAAu},{"mediumblue",0xFF0000CDu},{"mediumorchid",0xFFBA55D3u},
        {"mediumpurple",0xFF9370DBu},{"mediumseagreen",0xFF3CB371u},{"mediumslateblue",0xFF7B68EEu},
        {"mediumspringgreen",0xFF00FA9Au},{"mediumturquoise",0xFF48D1CCu},{"mediumvioletred",0xFFC71585u},
        {"midnightblue",0xFF191970u},{"mintcream",0xFFF5FFFAu},{"mistyrose",0xFFFFE4E1u},
        {"moccasin",0xFFFFE4B5u},{"navajowhite",0xFFFFDEADu},{"oldlace",0xFFFDF5E6u},
        {"olivedrab",0xFF6B8E23u},{"orangered",0xFFFF4500u},{"orchid",0xFFDA70D6u},
        {"palegoldenrod",0xFFEEE8AAu},{"palegreen",0xFF98FB98u},{"paleturquoise",0xFFAFEEEEu},
        {"palevioletred",0xFFDB7093u},{"papayawhip",0xFFFFEFD5u},{"peachpuff",0xFFFFDAB9u},
        {"peru",0xFFCD853Fu},{"pink",0xFFFFC0CBu},{"plum",0xFFDDA0DDu},{"powderblue",0xFFB0E0E6u},
        {"rebeccapurple",0xFF663399u},{"rosybrown",0xFFBC8F8Fu},{"royalblue",0xFF4169E1u},
        {"saddlebrown",0xFF8B4513u},{"salmon",0xFFFA8072u},{"sandybrown",0xFFF4A460u},
        {"seagreen",0xFF2E8B57u},{"seashell",0xFFFFF5EEu},{"sienna",0xFFA0522Du},
        {"skyblue",0xFF87CEEBu},{"slateblue",0xFF6A5ACDu},{"slategray",0xFF708090u},
        {"slategrey",0xFF708090u},{"snow",0xFFFFFAFAu},{"springgreen",0xFF00FF7Fu},
        {"steelblue",0xFF4682B4u},{"tan",0xFFD2B48Cu},{"thistle",0xFFD8BFD8u},
        {"tomato",0xFFFF6347u},{"turquoise",0xFF40E0D0u},{"violet",0xFFEE82EEu},
        {"wheat",0xFFF5DEB3u},{"whitesmoke",0xFFF5F5F5u},{"yellowgreen",0xFF9ACD32u},
        {"transparent",0x00000000u},
    };
    auto lookupNamed = [](const std::string& name) -> std::optional<uint32_t>
    {
        auto it = kNamedColors.find(name);
        if (it != kNamedColors.end())
            return it->second;
        return std::nullopt;
    };

    // rgb()/rgba() and hsl()/hsla() parsing helpers
    auto parseFloat = [](const std::string& t) -> std::optional<float>
    {
        try
        {
            size_t consumed = 0;
            const float value = std::stof(t, &consumed);
            if (consumed != t.size() || !std::isfinite(value)) return std::nullopt;
            return value;
        }
        catch (...) { return std::nullopt; }
    };
    auto parseChannel = [&](const std::string& t) -> std::optional<int>
    {
        if (!t.empty() && t.back() == '%')
        {
            auto sf = parseFloat(t.substr(0, t.size() - 1));
            if (!sf)
                return std::nullopt;
            return (int)std::round(std::clamp(*sf, 0.0f, 100.0f) * 2.55f);
        }
        auto sf = parseFloat(t);
        if (!sf)
            return std::nullopt;
        return (int)std::round(std::clamp(*sf, 0.0f, 255.0f));
    };
    auto parseAlpha = [&](const std::string& t) -> std::optional<int>
    {
        if (!t.empty() && t.back() == '%')
        {
            auto sf = parseFloat(t.substr(0, t.size() - 1));
            if (!sf)
                return std::nullopt;
            return (int)std::round(std::clamp(*sf, 0.0f, 100.0f) * 2.55f);
        }
        auto sf = parseFloat(t);
        if (!sf)
            return std::nullopt;
        return (int)std::round(std::clamp(*sf, 0.0f, 1.0f) * 255.0f);
    };

    auto splitArgs = [](std::string inner) -> std::vector<std::string>
    {
        // convert '/' to ',' and then split on commas; if none, split on whitespace
        for (char& c : inner)
        {
            if (c == '/')
                c = ',';
        }
        std::vector<std::string> out;
        size_t start = 0;
        bool anyComma = false;
        for (size_t i = 0; i < inner.size(); ++i)
        {
            if (inner[i] == ',')
            {
                out.push_back(Trim(inner.substr(start, i - start)));
                start = i + 1;
                anyComma = true;
            }
        }
        if (start <= inner.size())
        {
            out.push_back(Trim(inner.substr(start)));
        }
        if (!anyComma)
        { // split on whitespace
            std::vector<std::string> out2;
            std::istringstream ss(inner);
            std::string tok;
            while (ss >> tok)
                out2.push_back(tok);
            return out2;
        }
        return out;
    };

    // rgb/rgba
    if (sl.rfind("rgb(", 0) == 0 || sl.rfind("rgba(", 0) == 0)
    {
        size_t l = sl.find('('), r = sl.rfind(')');
        if (l != std::string::npos && r != std::string::npos && r > l && r + 1 == sl.size())
        {
            auto args = splitArgs(sl.substr(l + 1, r - l - 1));
            if (args.size() == 3 || args.size() == 4)
            {
                auto R = parseChannel(args[0]);
                auto G = parseChannel(args[1]);
                auto B = parseChannel(args[2]);
                int A = 255;
                if (args.size() >= 4)
                {
                    auto a = parseAlpha(args[3]);
                    if (!a) return std::nullopt;
                    A = *a;
                }
                if (R && G && B)
                    return toARGB(*R, *G, *B, A);
            }
        }
    }

    // hsl/hsla
    if (sl.rfind("hsl(", 0) == 0 || sl.rfind("hsla(", 0) == 0)
    {
        size_t l = sl.find('('), r = sl.rfind(')');
        if (l != std::string::npos && r != std::string::npos && r > l && r + 1 == sl.size())
        {
            auto args = splitArgs(sl.substr(l + 1, r - l - 1));
            if (args.size() == 3 || args.size() == 4)
            {
                auto Hf = parseFloat(args[0]);
                if (!Hf) return std::nullopt;
                float H = std::fmod(std::max(0.0f, *Hf), 360.0f) / 360.0f;
                auto Sp = args[1];
                auto Lp = args[2];
                if (!Sp.empty() && Sp.back() == '%')
                    Sp.pop_back();
                if (!Lp.empty() && Lp.back() == '%')
                    Lp.pop_back();
                auto Sopt = parseFloat(Sp);
                auto Lopt = parseFloat(Lp);
                if (Sopt && Lopt)
                {
                    float S = std::clamp(*Sopt / 100.0f, 0.0f, 1.0f);
                    float L = std::clamp(*Lopt / 100.0f, 0.0f, 1.0f);
                    float A = 1.0f;
                    if (args.size() >= 4)
                    {
                        auto a = parseAlpha(args[3]);
                        if (!a) return std::nullopt;
                        A = (*a) / 255.0f;
                    }
                    auto hue2rgb = [](float p, float q, float t)
                    { if(t<0) t+=1; if(t>1) t-=1; if(t<1.0f/6) return p+(q-p)*6*t; if(t<1.0f/2) return q; if(t<2.0f/3) return p+(q-p)*(2.0f/3 - t)*6; return p; };
                    float rf, gf, bf;
                    if (S == 0)
                    {
                        rf = gf = bf = L;
                    }
                    else
                    {
                        float q = L < 0.5f ? L * (1 + S) : L + S - L * S;
                        float p = 2 * L - q;
                        rf = hue2rgb(p, q, H + 1.0f / 3);
                        gf = hue2rgb(p, q, H);
                        bf = hue2rgb(p, q, H - 1.0f / 3);
                    }
                    return toARGB((int)std::round(rf * 255.0f), (int)std::round(gf * 255.0f), (int)std::round(bf * 255.0f), (int)std::round(A * 255.0f));
                }
            }
        }
    }

    // Hex formats: #RGB, #RRGGBB, #RRGGBBAA
    if (!sl.empty() && sl[0] == '#')
    {
        if (!std::all_of(sl.begin() + 1, sl.end(), [](unsigned char c) { return std::isxdigit(c); }))
            return std::nullopt;
        if (sl.size() == 4)
        {
            int r = (hex(sl[1]) << 4) | hex(sl[1]);
            int g = (hex(sl[2]) << 4) | hex(sl[2]);
            int b = (hex(sl[3]) << 4) | hex(sl[3]);
            return toARGB(r, g, b, 255);
        }
        else if (sl.size() == 7)
        {
            int r = (hex(sl[1]) << 4) | hex(sl[2]);
            int g = (hex(sl[3]) << 4) | hex(sl[4]);
            int b = (hex(sl[5]) << 4) | hex(sl[6]);
            return toARGB(r, g, b, 255);
        }
        else if (sl.size() == 9)
        {
            int r = (hex(sl[1]) << 4) | hex(sl[2]);
            int g = (hex(sl[3]) << 4) | hex(sl[4]);
            int b = (hex(sl[5]) << 4) | hex(sl[6]);
            int a = (hex(sl[7]) << 4) | hex(sl[8]);
            return toARGB(r, g, b, a);
        }
    }

    // 0xAARRGGBB
    if (sl.size() > 2 && sl[0] == '0' && (sl[1] == 'x' || sl[1] == 'X'))
    {
        try
        {
            if (sl.size() > 10 || !std::all_of(sl.begin() + 2, sl.end(),
                    [](unsigned char c) { return std::isxdigit(c); })) return std::nullopt;
            return static_cast<uint32_t>(std::stoul(sl, nullptr, 16));
        }
        catch (...)
        {
        }
    }

    // Fast-path CSS named colors (including "transparent") before decimal parse so we
    // don't rely on exceptions-as-control-flow when parsing theme.css keywords.
    if (auto nc = lookupNamed(sl))
        return *nc;

    // Decimal ARGB or RGB? treat as ARGB if >= 0x01000000. To keep debugger noise
    // down when first-chance C++ exceptions are enabled, avoid calling std::stoul
    // on obviously non-numeric tokens such as "transparent".
    bool decimalLike = !sl.empty();
    if (decimalLike)
    {
        for (char c : sl)
        {
            if (c < '0' || c > '9')
            {
                decimalLike = false;
                break;
            }
        }
    }
    if (decimalLike)
    {
        try
        {
            const auto dec = std::stoull(sl, nullptr, 10);
            if (dec >= 0x01000000u && dec <= 0xffffffffu)
                return static_cast<uint32_t>(dec);
        }
        catch (...)
        {
        }
    }

    return std::nullopt;
}

// A declaration's colour; an unreadable one falls back to opaque white.
static uint32_t ParseColor(const std::string& v)
{
    return TryParseColorValue(v).value_or(0xFFFFFFFFu);
}

static float ParseFloatPx(const std::string& v, bool* ok = nullptr)
{
    if (ok)
        *ok = false;

    const std::string s = Trim(v);
    if (s.empty())
        return 0.f;

    // Avoid exceptions-as-control-flow (e.g. std::stof("solid") throws). This makes
    // parsing robust under debuggers that break on first-chance C++ exceptions.
    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    float f = std::strtof(begin, &end);
    if (end == begin)
        return 0.f; // no numeric prefix

    // Returns the NUMERIC PREFIX only; the unit suffix is not interpreted here and
    // an unknown unit is not rejected. Callers that care about the unit — every
    // property whose value can be a percentage — inspect a single already-
    // tokenized component themselves and build a StyleLength (ParseLengthValue,
    // ParseRadiusToken, ParseGapToken, ParseFontSizeValue). A caller that reads
    // this number alone is declaring that its property is pixels-only.
    std::string suf = end ? Trim(std::string(end)) : std::string{};
    (void)suf;

    if (ok)
        *ok = true;
    return f;
}

static float ParseTimeSec(const std::string& raw, bool* ok = nullptr)
{
    if (ok)
        *ok = false;
    std::string s = Trim(raw);
    if (s.empty())
        return 0.0f;

    bool isMs = false;
    if (s.size() >= 2 && s[s.size() - 2] == 'm' &&
        (s.back() == 's' || s.back() == 'S'))
    {
        isMs = true;
        s.resize(s.size() - 2);
    }
    else if (!s.empty() && (s.back() == 's' || s.back() == 'S'))
    {
        s.pop_back();
    }
    else
    {
        return 0.0f;
    }

    const char* begin = s.c_str();
    char* end = nullptr;
    errno = 0;
    float f = std::strtof(begin, &end);
    if (end == begin)
        return 0.0f;

    if (ok)
        *ok = true;
    return isMs ? f * 0.001f : f;
}

static Math::EasingFunction ParseEasingKeyword(const std::string& s)
{
    if (Ieq(s, "linear"))       return Math::EasingFunction::Linear;
    if (Ieq(s, "ease"))         return Math::EasingFunction::Ease;
    if (Ieq(s, "ease-in"))      return Math::EasingFunction::EaseIn;
    if (Ieq(s, "ease-out"))     return Math::EasingFunction::EaseOut;
    if (Ieq(s, "ease-in-out"))  return Math::EasingFunction::EaseInOut;
    return Math::EasingFunction::Ease;
}

static bool IsEasingKeyword(const std::string& s)
{
    return Ieq(s, "linear") || Ieq(s, "ease") || Ieq(s, "ease-in") ||
           Ieq(s, "ease-out") || Ieq(s, "ease-in-out");
}

// ---------------------------------------------------------------------------
// Property descriptor table. One row per accepted declaration name; aliases
// are separate rows sharing an id + parser. Adding a CSS property means adding
// its parse (or expand) function and one table row — the static_asserts below
// fail the build if a StylePropertyId is left without a row.
// ---------------------------------------------------------------------------

static std::vector<std::string> TokenizeWhitespaceOutsideParens(const std::string& value);
static bool SplitAtTopLevelSlash(const std::string& value, std::string& before, std::string& after);
static void AppendBorderWidthLonghands(float top, float right, float bottom, float left, std::vector<StyleProperty>& outProps);
static void AppendBorderColorLonghands(uint32_t top, uint32_t right, uint32_t bottom, uint32_t left, std::vector<StyleProperty>& outProps);
static void AppendBorderRadiusLonghands(const CornerRadiusValue& tl, const CornerRadiusValue& tr, const CornerRadiusValue& br, const CornerRadiusValue& bl, std::vector<StyleProperty>& outProps);

using ParseValueFn = StyleValue (*)(const std::string& value);
using ExpandShorthandFn = void (*)(const std::string& value, std::vector<StyleProperty>& outProps);

// Id may be Unknown for pure shorthands ("border", "flex") that exist only as
// expansions: the transition parser (FindPropertyRow at its property-name
// token) reads a matched row's Unknown Id as "all properties".
struct PropertyRow
{
    std::string_view Name; // lowercase (asserted below); matched case-insensitively
    StylePropertyId Id = StylePropertyId::Unknown;
    ParseValueFn Parse = nullptr;
    ExpandShorthandFn Expand = nullptr;              // checked before Parse
    std::span<const StylePropertyId> KeywordTargets; // CSS-wide keyword fan-out; empty -> Id
};

static const PropertyRow* FindPropertyRow(const std::string& name);

// --- shared category parsers ------------------------------------------------

static StyleValue ParseFloatPxValue(const std::string& value)
{
    return ParseFloatPx(value);
}

static StyleValue ParseFloat01Value(const std::string& value)
{
    return std::clamp(ParseFloatPx(value), 0.0f, 1.0f);
}

static StyleValue ParseColorValue(const std::string& value)
{
    return ParseColor(Trim(value));
}

// css-values-3 math functions — calc(), min(), max() — constant-folded at
// parse time. This is sound because var() substitution is textual and runs
// before value parsing (the deferred-declaration pass), so by the time a
// declaration reaches the length parser, `calc(12px * var(--length-scale))`
// reads `calc(12px * 1.375)`. Only same-type arithmetic can fold without a
// layout-time calc tree, so a px/percent mix ("calc(50% - 4px)") is rejected
// whole, leaving the cascaded value standing — the same failure shape every
// other unreadable declaration has here.
namespace CssMath
{
struct Value
{
    enum class Kind : uint8_t { Number, Px, Percent };
    Kind K = Kind::Number;
    float V = 0.0f;
};

class Parser
{
  public:
    explicit Parser(std::string_view s) : m_S(s) {}

    bool ParseFunction(Value& out)
    {
        SkipWs();
        if (!ParseMathFunction(out))
            return false;
        SkipWs();
        return m_Pos == m_S.size();
    }

  private:
    /* Stylesheets load from user projects: bound the recursion so a
       parenthesis bomb cannot blow the stack. Real declarations nest 2-3. */
    static constexpr int kMaxDepth = 32;

    std::string_view m_S;
    size_t m_Pos = 0;
    int m_Depth = 0;

    void SkipWs()
    {
        while (m_Pos < m_S.size() && (m_S[m_Pos] == ' ' || m_S[m_Pos] == '\t' ||
                                      m_S[m_Pos] == '\n' || m_S[m_Pos] == '\r'))
            ++m_Pos;
    }
    bool Eat(char c)
    {
        if (m_Pos < m_S.size() && m_S[m_Pos] == c)
        {
            ++m_Pos;
            return true;
        }
        return false;
    }
    bool EatWord(const char* w)
    {
        const size_t n = std::strlen(w);
        if (m_S.compare(m_Pos, n, w) == 0)
        {
            m_Pos += n;
            return true;
        }
        return false;
    }

    bool ParseMathFunction(Value& out)
    {
        if (EatWord("calc("))
        {
            if (!ParseSum(out))
                return false;
            SkipWs();
            return Eat(')');
        }
        const bool isMin = EatWord("min(");
        const bool isMax = !isMin && EatWord("max(");
        if (!isMin && !isMax)
            return false;
        Value acc;
        if (!ParseSum(acc))
            return false;
        for (;;)
        {
            SkipWs();
            if (Eat(')'))
            {
                out = acc;
                return true;
            }
            if (!Eat(','))
                return false;
            Value next;
            if (!ParseSum(next))
                return false;
            if (next.K != acc.K)
                return false; // arguments must agree in type
            if (isMin ? (next.V < acc.V) : (next.V > acc.V))
                acc = next;
        }
    }

    // sum := product (('+' | '-') product)*. The spec requires whitespace on
    // both sides of '+' and '-' so "1-2px" stays a dimension, not arithmetic.
    bool ParseSum(Value& out)
    {
        if (!ParseProduct(out))
            return false;
        for (;;)
        {
            const size_t save = m_Pos;
            SkipWs();
            const bool spacedBefore = m_Pos != save;
            if (m_Pos >= m_S.size() || (m_S[m_Pos] != '+' && m_S[m_Pos] != '-'))
            {
                m_Pos = save;
                return true;
            }
            const char op = m_S[m_Pos];
            const size_t afterOp = m_Pos + 1;
            const bool spacedAfter =
                afterOp < m_S.size() && (m_S[afterOp] == ' ' || m_S[afterOp] == '\t');
            if (!spacedBefore || !spacedAfter)
            {
                m_Pos = save;
                return true; // not sum syntax; let the caller hit the terminator
            }
            m_Pos = afterOp;
            Value rhs;
            if (!ParseProduct(rhs))
                return false;
            if (rhs.K != out.K)
                return false; // '+'/'-' need matching types to fold
            out.V = (op == '+') ? out.V + rhs.V : out.V - rhs.V;
        }
    }

    bool ParseProduct(Value& out)
    {
        if (!ParseUnit(out))
            return false;
        for (;;)
        {
            const size_t save = m_Pos;
            SkipWs();
            if (m_Pos >= m_S.size() || (m_S[m_Pos] != '*' && m_S[m_Pos] != '/'))
            {
                m_Pos = save;
                return true;
            }
            const char op = m_S[m_Pos++];
            Value rhs;
            if (!ParseUnit(rhs))
                return false;
            if (op == '*')
            {
                if (out.K == Value::Kind::Number)
                {
                    out.K = rhs.K;
                    out.V *= rhs.V;
                }
                else if (rhs.K == Value::Kind::Number)
                {
                    out.V *= rhs.V;
                }
                else
                {
                    return false; // length × length has no CSS type
                }
            }
            else
            {
                if (rhs.K != Value::Kind::Number || rhs.V == 0.0f)
                    return false; // division only by a non-zero number
                out.V /= rhs.V;
            }
        }
    }

    bool ParseUnit(Value& out)
    {
        if (m_Depth >= kMaxDepth)
            return false;
        ++m_Depth;
        struct DepthGuard
        {
            int& D;
            ~DepthGuard() { --D; }
        } guard{m_Depth};

        SkipWs();
        if (Eat('('))
        {
            if (!ParseSum(out))
                return false;
            SkipWs();
            return Eat(')');
        }
        if (m_S.compare(m_Pos, 5, "calc(") == 0 || m_S.compare(m_Pos, 4, "min(") == 0 ||
            m_S.compare(m_Pos, 4, "max(") == 0)
        {
            return ParseMathFunction(out);
        }

        // <number> with an optional px / % suffix. Any other unit is rejected:
        // folding cannot guess what an unknown dimension means.
        const size_t start = m_Pos;
        if (m_Pos < m_S.size() && (m_S[m_Pos] == '+' || m_S[m_Pos] == '-'))
            ++m_Pos;
        bool sawDigit = false;
        while (m_Pos < m_S.size() &&
               ((m_S[m_Pos] >= '0' && m_S[m_Pos] <= '9') || m_S[m_Pos] == '.'))
        {
            sawDigit = sawDigit || (m_S[m_Pos] >= '0' && m_S[m_Pos] <= '9');
            ++m_Pos;
        }
        if (!sawDigit)
            return false;
        // strtof: macOS libc++ has no float from_chars before 26.0. The
        // substring is sign/digits/dot only, so full consumption is the
        // validity check.
        const std::string num(m_S.substr(start, m_Pos - start));
        char* endPtr = nullptr;
        const float parsed = std::strtof(num.c_str(), &endPtr);
        if (endPtr != num.c_str() + num.size())
            return false;
        out.V = parsed;
        if (Eat('%'))
            out.K = Value::Kind::Percent;
        else if (EatWord("px"))
            out.K = Value::Kind::Px;
        else
            out.K = Value::Kind::Number;
        return true;
    }
};

// Evaluate a whole calc()/min()/max() token to a StyleLength. Numbers are not
// lengths, so a numeric result is invalid in a length context.
bool EvalToLength(std::string_view token, StyleLength& out)
{
    Value v;
    Parser parser(token);
    if (!parser.ParseFunction(v))
        return false;
    switch (v.K)
    {
    case Value::Kind::Px:
        out = StyleLength::Px(v.V);
        return true;
    case Value::Kind::Percent:
        out = StyleLength::Percent(v.V);
        return true;
    case Value::Kind::Number:
        return false;
    }
    return false;
}

bool IsMathFunctionToken(std::string_view v)
{
    return v.rfind("calc(", 0) == 0 || v.rfind("min(", 0) == 0 || v.rfind("max(", 0) == 0;
}
} // namespace CssMath

// Every longhand whose value is a single <length-percentage> or `auto`: the
// sizing properties (css-sizing-3 §5), the padding and margin edges (css-box-3
// §4), the inset properties (css-position-3 §3) and flex-basis. All take
// exactly one component, so a second one makes the declaration invalid: it is
// rejected whole, which leaves the cascaded value standing (CSS 2.1 §4.2).
// Rejecting the extra token is also what keeps the percent sniff honest — read
// over the whole declaration it would take `width: 10px 25%` for Percent(10).
//
// `auto` cannot double as the answer for anything unreadable, because it is a
// value in its own right on every one of these properties: the initial value of
// the sizing and inset properties, the free-space absorber on a margin edge,
// and — CSS having no `padding: auto` — a collapse to zero on a padding edge
// (CSSParser applyBoxEdge). Math functions fold here (CssMath above); a calc
// that cannot fold — mixed px/percent, unknown units — is rejected rather than
// resolved to Auto, and the author's earlier rule survives instead of being
// overwritten with a value they never wrote.
static StyleValue ParseLengthValue(const std::string& raw)
{
    const auto toks = TokenizeWhitespaceOutsideParens(raw);
    if (toks.size() != 1)
        return std::monostate{};
    const std::string v = ToLowerAscii(Trim(toks[0]));
    if (v == "auto")
        return StyleLength::Auto();
    if (CssMath::IsMathFunctionToken(v))
    {
        StyleLength folded{};
        if (CssMath::EvalToLength(v, folded))
            return folded;
        return std::monostate{};
    }
    bool ok = false;
    const bool isPercent = (!v.empty() && v.back() == '%');
    const float f = ParseFloatPx(v, &ok);
    if (!ok)
        return std::monostate{};
    return isPercent ? StyleLength::Percent(f) : StyleLength::Px(f);
}

// max-width / max-height, whose extra keyword is `none`. StyleLength has no
// "no maximum" unit, so `none` lands on Auto, which the layout reads as
// unconstrained.
static StyleValue ParseMaxLengthValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    if (v == "none" || v == "auto")
        return StyleLength::Auto();
    return ParseLengthValue(value);
}

// One corner radius component: a length or a percentage, never `auto`. A
// percentage refers to the corresponding dimension of the border box
// (css-backgrounds-3 §5.1), which is not known until layout, so it cannot be
// flattened to px here. `tok` must be a single already-tokenized component: the
// percent sniff reads the token's last character, and over a wider string it
// would take another component's unit for this one's.
static bool ParseRadiusToken(const std::string& tok, StyleLength& out)
{
    const std::string v = ToLowerAscii(Trim(tok));
    if (v.empty() || v == "auto")
        return false;
    bool ok = false;
    const bool isPercent = (v.back() == '%');
    const float f = ParseFloatPx(v, &ok);
    if (!ok)
        return false;
    out = isPercent ? StyleLength::Percent(f) : StyleLength::Px(f);
    return true;
}

// border-*-radius longhands: `<length-percentage>{1,2}` — the horizontal radius
// then the vertical, which together name an ELLIPSE (css-backgrounds-3 §5.1).
// A single value is a circular corner: the vertical radius defaults to the
// horizontal one. Both components travel in one CornerRadiusValue so the
// corner keeps the pair; the two can carry different units (`10px 20%`), which
// is why neither resolves before layout.
//
// An `auto`, an unreadable component, or a third one rejects the declaration,
// leaving the cascaded value standing (CSS 2.1 §4.2).
static StyleValue ParseRadiusLengthValue(const std::string& raw)
{
    const auto toks = TokenizeWhitespaceOutsideParens(raw);
    if (toks.empty() || toks.size() > 2)
        return std::monostate{};

    StyleLength horizontal{};
    if (!ParseRadiusToken(toks[0], horizontal))
        return std::monostate{};

    StyleLength vertical = horizontal;
    if (toks.size() == 2 && !ParseRadiusToken(toks[1], vertical))
        return std::monostate{};

    return CornerRadiusValue{horizontal, vertical};
}

// One gap gutter: a length or a percentage, never `auto`. A percentage refers
// to the element's own CONTENT box in the gutter's axis (css-align-3 §8.1) —
// not to the parent — which is not known until layout, so it cannot be
// flattened to px here. `tok` must be a single already-tokenized component:
// the percent sniff reads the token's last character, and over a wider string
// it would take another component's unit for this one's.
static bool ParseGapToken(const std::string& tok, StyleLength& out)
{
    const std::string v = ToLowerAscii(Trim(tok));
    if (v.empty() || v == "auto")
        return false;
    bool ok = false;
    const bool isPercent = (v.back() == '%');
    const float f = ParseFloatPx(v, &ok);
    if (!ok)
        return false;
    out = isPercent ? StyleLength::Percent(f) : StyleLength::Px(f);
    return true;
}

// row-gap / column-gap: exactly one gutter component. Anything else — extra
// tokens included — is rejected, leaving the cascaded value standing
// (CSS 2.1 §4.2). The `gap` shorthand is ExpandGapValue.
static StyleValue ParseGapLengthValue(const std::string& raw)
{
    const auto toks = TokenizeWhitespaceOutsideParens(raw);
    StyleLength len{};
    if (toks.size() != 1 || !ParseGapToken(toks[0], len))
        return std::monostate{};
    return len;
}

// font-size: one length or percentage. A percentage is relative to the PARENT
// element's computed font size (css-fonts-4 §3.5) — not to this element's box —
// so it cannot be resolved until the parent is, and has to travel with its unit.
// The absolute and relative size keywords (`medium`, `larger`, …) are not
// implemented; rejecting them — like any extra token, whose unit the last-
// character percent sniff would otherwise claim — leaves the cascaded value
// standing (CSS 2.1 §4.2), which beats reading `larger` as the 0 its numeric
// prefix parses to.
static StyleValue ParseFontSizeValue(const std::string& raw)
{
    const auto toks = TokenizeWhitespaceOutsideParens(raw);
    if (toks.size() != 1)
        return std::monostate{};
    const std::string v = ToLowerAscii(Trim(toks[0]));
    if (v.empty() || v == "auto")
        return std::monostate{};
    bool ok = false;
    const bool isPercent = (v.back() == '%');
    const float f = ParseFloatPx(v, &ok);
    if (!ok)
        return std::monostate{};
    return isPercent ? StyleLength::Percent(f) : StyleLength::Px(f);
}

// margin/padding shorthand: 1-4 values, each a length, a percentage, or (margin
// only) `auto`. Padding has no `auto` in CSS, so the padding appliers read
// Values/IsPercent and ignore IsAuto, leaving the zero.
static StyleValue ParseBoxValue(const std::string& value)
{
    struct Edge
    {
        float Value = 0.0f;
        bool IsPercent = false;
        bool IsAuto = false;
    };

    std::istringstream ss(value);
    std::string tok;
    std::vector<Edge> edges;
    while (ss >> tok)
    {
        Edge edge{};
        edge.IsAuto = Ieq(tok, "auto");
        edge.IsPercent = !edge.IsAuto && !tok.empty() && tok.back() == '%';
        edge.Value = edge.IsAuto ? 0.0f : ParseFloatPx(tok);
        edges.push_back(edge);
    }

    StyleBox box{};
    if (edges.empty())
        return box;

    // CSS shorthand fan-out: 1 value sets all four edges, 2 set [TB, RL],
    // 3 set [T, RL, B], 4 or more set [T, R, B, L].
    const Edge& top = edges[0];
    const Edge& right = edges.size() >= 2 ? edges[1] : edges[0];
    const Edge& bottom = edges.size() >= 3 ? edges[2] : edges[0];
    const Edge& left = edges.size() >= 4 ? edges[3] : right;

    box.Values = Box4{top.Value, right.Value, bottom.Value, left.Value};
    box.IsPercent = Box4b{top.IsPercent, right.IsPercent, bottom.IsPercent, left.IsPercent};
    box.IsAuto = Box4b{top.IsAuto, right.IsAuto, bottom.IsAuto, left.IsAuto};
    return box;
}

// --- per-property parsers -----------------------------------------------

static StyleValue ParseDisplayValue(const std::string& value)
{
    if (Ieq(value, "none"))
        return DisplayMode::None;
    // Yoga only distinguishes between flex and none; inline-flex behaves the
    // same as flex for layout purposes in this engine.
    if (Ieq(value, "flex") || Ieq(value, "inline-flex"))
        return DisplayMode::Flex;
    if (Ieq(value, "inline"))
        return DisplayMode::Inline;
    return DisplayMode::Block;
}

static StyleValue ParseVisibilityValue(const std::string& value)
{
    // `collapse` hides a non-table element exactly as `hidden` does.
    return !Ieq(value, "hidden") && !Ieq(value, "collapse");
}

static StyleValue ParsePointerEventsValue(const std::string& value)
{
    return ToLowerAscii(Trim(value)) != "none";
}

static StyleValue ParseOverflowValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    // `overlay` is a legacy alias of `auto`: it scrolls, so it clips.
    return (v == "hidden" || v == "clip" || v == "scroll" || v == "auto" || v == "overlay")
               ? Overflow::Hidden
               : Overflow::Visible;
}

// overflow-x/overflow-y accept a narrower keyword set than the shorthand:
// scroll/auto stay Visible per-axis.
static StyleValue ParseOverflowAxisValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    return (v == "hidden" || v == "clip") ? Overflow::Hidden : Overflow::Visible;
}

// Unmodeled styles (double/groove/ridge/...) render as Solid; none/hidden
// suppress the stroke entirely.
static StyleValue ParseBorderStyleValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    if (v == "dotted")
        return BorderStyle::Dotted;
    if (v == "dashed")
        return BorderStyle::Dashed;
    if (v == "none" || v == "hidden")
        return BorderStyle::None;
    return BorderStyle::Solid;
}

// The ring is one stroked rect, so only none/solid are distinguishable today.
// `auto` is the UA-chosen ring, which for this engine is the solid one.
// Dashed/dotted outlines would need the dash/dot emitters retargeted at the
// outline rect; no shipped stylesheet asks for them, so they render solid.
static StyleValue ParseOutlineStyleValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    if (v == "none" || v == "hidden")
        return BorderStyle::None;
    return BorderStyle::Solid;
}

// outline-width accepts the CSS line-width keywords as well as a length.
// Negative widths are invalid in CSS; clamp rather than invert the ring.
static StyleValue ParseLineWidthValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    if (v == "thin")
        return kThinLineWidthPx;
    if (v == "medium")
        return kMediumLineWidthPx;
    if (v == "thick")
        return kThickLineWidthPx;
    return std::max(0.0f, ParseFloatPx(v));
}

static StyleValue ParseCursorValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    CursorStyle cursor = CursorStyle::Auto;
    if (v == "pointer" || v == "hand")
        cursor = CursorStyle::Pointer;
    else if (v == "text")
        cursor = CursorStyle::Text;
    else if (v == "crosshair")
        cursor = CursorStyle::Crosshair;
    else if (v == "move")
        cursor = CursorStyle::Move;
    else if (v == "col-resize" || v == "ew-resize")
        cursor = CursorStyle::ColResize;
    else if (v == "row-resize" || v == "ns-resize")
        cursor = CursorStyle::RowResize;
    // No north-east/south-west diagonal exists; only the NW-SE pair maps.
    else if (v == "nwse-resize" || v == "nw-resize" || v == "se-resize")
        cursor = CursorStyle::NorthwestSoutheastResize;
    else if (v == "not-allowed" || v == "no-drop")
        cursor = CursorStyle::NotAllowed;
    else if (v == "grab")
        cursor = CursorStyle::Grab;
    else if (v == "grabbing")
        cursor = CursorStyle::Grabbing;
    return cursor;
}

static StyleValue ParseTextAlignValue(const std::string& value)
{
    const std::string v = Trim(value);
    if (Ieq(v, "center"))
        return TextAlign::Center;
    if (Ieq(v, "right"))
        return TextAlign::Right;
    // `start`/`end` resolve against the inline direction; nothing reads
    // `direction` here yet, so `end` resolves as it would under LTR.
    if (Ieq(v, "end"))
        return TextAlign::Right;
    return TextAlign::Left;
}

static StyleValue ParseWordBreakValue(const std::string& value)
{
    const std::string v = Trim(value);
    if (Ieq(v, "break-all"))
        return WordBreak::BreakAll;
    if (Ieq(v, "keep-all"))
        return WordBreak::KeepAll;
    if (Ieq(v, "break-word"))
        return WordBreak::BreakWord;
    return WordBreak::Normal;
}

static StyleValue ParseOverflowWrapValue(const std::string& value)
{
    const std::string v = Trim(value);
    // `anywhere` differs from `break-word` only in the min-content
    // contribution, which layout here does not compute separately.
    if (Ieq(v, "break-word") || Ieq(v, "anywhere"))
        return OverflowWrap::BreakWord;
    return OverflowWrap::Normal;
}

static StyleValue ParseWhiteSpaceValue(const std::string& value)
{
    const std::string v = Trim(value);
    if (Ieq(v, "nowrap") || Ieq(v, "no-wrap"))
        return WhiteSpace::NoWrap;
    if (Ieq(v, "pre"))
        return WhiteSpace::Pre;
    // pre-wrap / pre-line fall back to Normal (wrapping preserved).
    return WhiteSpace::Normal;
}

static StyleValue ParseTextOverflowValue(const std::string& value)
{
    const std::string v = Trim(value);
    if (Ieq(v, "ellipsis"))
        return TextOverflowMode::Ellipsis;
    // clip, and anything unrecognized (custom ellipsis strings), paints as clip.
    return TextOverflowMode::Clip;
}

static StyleValue ParseLineHeightValue(const std::string& value)
{
    // 0 is the resolver's "normal" sentinel. Recognise the keyword explicitly:
    // it otherwise lands there only because the numeric parse fails and the
    // unit scan happens to see letters.
    if (ToLowerAscii(Trim(value)) == "normal")
        return 0.0f;

    bool ok = false;
    float f = ParseFloatPx(value, &ok);
    if (!ok)
        f = 0.0f;

    // Distinguish unitless multiplier from pixel value. CSS "line-height: 1.5"
    // means 1.5x the font size, while "line-height: 21px" means 21 pixels.
    // Store unitless multipliers as negative; consumers check the sign.
    const std::string trimmed = Trim(value);
    bool hasUnit = false;
    for (char c : trimmed)
    {
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '%')
        {
            hasUnit = true;
            break;
        }
    }
    if (!hasUnit && f > 0.0f)
        f = -f;
    return f;
}

static StyleValue ParseLetterSpacingValue(const std::string& value)
{
    // `normal` is the initial value and computes to zero extra spacing.
    // Recognise the keyword explicitly (see ParseLineHeightValue). Lengths are
    // px, like font-size; negative values legitimately tighten tracking.
    if (ToLowerAscii(Trim(value)) == "normal")
        return 0.0f;

    bool ok = false;
    const float f = ParseFloatPx(value, &ok);
    return ok ? f : 0.0f;
}

static StyleValue ParseFontFamilyValue(const std::string& value)
{
    // Split on commas, trim spaces and quotes.
    std::vector<std::string> fams;
    std::stringstream ss(value);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        tok = Trim(tok);
        if (!tok.empty() && tok.front() == '"' && tok.back() == '"' && tok.size() >= 2)
            tok = tok.substr(1, tok.size() - 2);
        if (!tok.empty() && tok.front() == '\'' && tok.back() == '\'' && tok.size() >= 2)
            tok = tok.substr(1, tok.size() - 2);
        if (!tok.empty())
            fams.push_back(tok);
    }
    return fams;
}

static StyleValue ParseFontWeightValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    int weight = 400;
    if (v == "bold" || v == "bolder")
        weight = 700; // best-effort (no parent-relative resolution yet)
    else if (v == "lighter")
        weight = 300; // best-effort
    else if (v != "normal")
    {
        // Numeric 1..1000 (CSS commonly uses 100..900)
        try
        {
            weight = std::clamp(std::stoi(v), 1, 1000);
        }
        catch (...)
        {
            weight = 400;
        }
    }
    return weight;
}

static StyleValue ParseFontStyleValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    if (v == "italic")
        return FontStyle::Italic;
    if (v == "oblique")
        return FontStyle::Oblique;
    return FontStyle::Normal;
}

static StyleValue ParseFontVariantValue(const std::string& value)
{
    return (ToLowerAscii(Trim(value)) == "small-caps") ? FontVariant::SmallCaps
                                                       : FontVariant::Normal;
}

static StyleValue ParseFlexDirectionValue(const std::string& value)
{
    // The engine does not model reversed flow, so the -reverse forms keep their
    // axis and lose only the direction.
    return (Ieq(value, "row") || Ieq(value, "row-reverse")) ? FlexDirection::Row
                                                            : FlexDirection::Column;
}

static StyleValue ParseFlexWrapValue(const std::string& value)
{
    return Ieq(value, "wrap") || Ieq(value, "wrap-reverse");
}

static StyleValue ParseJustifyContentValue(const std::string& value)
{
    if (Ieq(value, "center"))
        return JustifyContent::Center;
    // Logical and physical aliases collapse onto the flex-relative pair; see
    // ParseAlignKeyword for why the distinction does not survive here.
    if (Ieq(value, "flex-end") || Ieq(value, "end") || Ieq(value, "right"))
        return JustifyContent::FlexEnd;
    if (Ieq(value, "space-between"))
        return JustifyContent::SpaceBetween;
    if (Ieq(value, "space-around"))
        return JustifyContent::SpaceAround;
    if (Ieq(value, "space-evenly"))
        return JustifyContent::SpaceEvenly;
    return JustifyContent::FlexStart;
}

// Keyword set shared by align-items (css-align-3 6.2) and align-self (7.1).
// `normal` behaves as `stretch` on flex items (6.2.4). The logical
// (`start`/`end`) and self-relative (`self-start`/`self-end`) families collapse
// onto the flex-relative pair: this engine resolves no writing mode, so they
// only differ under RTL, which Yoga applies from the container's direction.
// nullopt means unrecognised - the caller drops the declaration rather than
// substituting some other keyword's value.
static std::optional<AlignItems> ParseAlignKeyword(const std::string& value)
{
    const std::string v = Trim(value);
    if (Ieq(v, "normal") || Ieq(v, "stretch"))
        return AlignItems::Stretch;
    if (Ieq(v, "flex-start") || Ieq(v, "start") || Ieq(v, "self-start"))
        return AlignItems::FlexStart;
    if (Ieq(v, "center"))
        return AlignItems::Center;
    if (Ieq(v, "flex-end") || Ieq(v, "end") || Ieq(v, "self-end"))
        return AlignItems::FlexEnd;
    if (Ieq(v, "baseline"))
        return AlignItems::Baseline;
    return std::nullopt;
}

static StyleValue ParseAlignItemsValue(const std::string& value)
{
    const std::optional<AlignItems> align = ParseAlignKeyword(value);
    return align ? StyleValue{*align} : StyleValue{};
}

static StyleValue ParseAlignSelfValue(const std::string& value)
{
    if (Ieq(Trim(value), "auto"))
        return AlignItems::Auto;
    const std::optional<AlignItems> align = ParseAlignKeyword(value);
    return align ? StyleValue{*align} : StyleValue{};
}

static StyleValue ParseAlignContentValue(const std::string& value)
{
    if (Ieq(value, "flex-start") || Ieq(value, "start"))
        return AlignContent::FlexStart;
    if (Ieq(value, "center"))
        return AlignContent::Center;
    if (Ieq(value, "flex-end") || Ieq(value, "end"))
        return AlignContent::FlexEnd;
    if (Ieq(value, "space-between"))
        return AlignContent::SpaceBetween;
    if (Ieq(value, "space-around"))
        return AlignContent::SpaceAround;
    if (Ieq(value, "space-evenly"))
        return AlignContent::SpaceEvenly;
    return AlignContent::Stretch;
}

static StyleValue ParseDirectionValue(const std::string& value)
{
    if (Ieq(value, "rtl"))
        return Direction::RTL;
    if (Ieq(value, "ltr"))
        return Direction::LTR;
    return Direction::Inherit;
}

static StyleValue ParsePositionValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    // `fixed` is viewport-anchored; with no viewport-relative containing block
    // modelled, out-of-flow absolute is the closest behaviour.
    return (v == "absolute" || v == "fixed") ? PositionType::Absolute : PositionType::Relative;
}

static StyleValue ParseOrderValue(const std::string& value)
{
    const std::string v = Trim(value);
    try
    {
        return std::stoi(v);
    }
    catch (...)
    {
    }
    try
    {
        return static_cast<int>(std::round(std::stof(v)));
    }
    catch (...)
    {
    }
    return 0;
}

static StyleValue ParseZIndexValue(const std::string& value)
{
    bool ok = false;
    const float f = ParseFloatPx(Trim(value), &ok);
    // Fractional z-index values (e.g. 1.7) round to the nearest integer.
    return ok ? static_cast<int>(std::round(f)) : 0;
}

static StyleValue ParseAspectRatioValue(const std::string& value)
{
    const std::string v = RemoveWhitespace(ToLowerAscii(value));
    float ratio = 0.0f;
    if (!Ieq(v, "auto") && !v.empty())
    {
        const size_t slash = v.find('/');
        try
        {
            if (slash != std::string::npos)
            {
                const float w = std::stof(v.substr(0, slash));
                const float h = std::stof(v.substr(slash + 1));
                ratio = (h > 0.0f) ? (w / h) : 0.0f;
            }
            else
            {
                ratio = std::stof(v);
            }
        }
        catch (...)
        {
            ratio = 0.0f;
        }
    }
    return ratio;
}

static StyleValue ParseBackgroundImageValue(const std::string& value)
{
    const std::string v = Trim(value);
    BackgroundImageSource src{};
    if (!Ieq(v, "none"))
    {
        // Support engine(name) indirection for external textures registered in UIManager
        std::string lv = ToLowerAscii(v);
        if (lv.rfind("engine(", 0) == 0 && v.back() == ')')
        {
            auto l = v.find('(');
            auto r = v.rfind(')');
            std::string inner = (l != std::string::npos && r != std::string::npos && r > l) ? Trim(v.substr(l + 1, r - l - 1)) : std::string();
            if (!inner.empty() && (inner.front() == '"' || inner.front() == '\''))
                inner = inner.substr(1, inner.size() - 2);
            src.Kind = BackgroundImageSource::SourceKind::ResourceName;
            src.Value = inner;
        }
        else
        {
            // Expect url(...) possibly quoted. Accept guid:... or project-relative path
            auto l = v.find('(');
            auto r = v.rfind(')');
            std::string inner = (l != std::string::npos && r != std::string::npos && r > l) ? Trim(v.substr(l + 1, r - l - 1)) : v;
            if (!inner.empty() && (inner.front() == '"' || inner.front() == '\''))
                inner = inner.substr(1, inner.size() - 2);
            // Strip optional url: prefix if present
            if (inner.rfind("url:", 0) == 0)
                inner = inner.substr(4);
            inner = Trim(inner);
            // Optional explicit asset-source alias: "editor:path" or
            // "@editor/path". Lets a stylesheet escape the implicit
            // source-priority resolution and target a specific mount
            // (typically when project CSS needs an editor icon, or
            // vice versa). Detected before guid/path classification
            // so it composes cleanly with both.
            std::string sourceAlias;
            {
                auto isAliasIdent = [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                           || (c >= '0' && c <= '9') || c == '_' || c == '-';
                };
                if (!inner.empty() && inner.front() == '@')
                {
                    // @alias/path
                    size_t slash = inner.find('/');
                    if (slash != std::string::npos && slash > 1)
                    {
                        std::string a = inner.substr(1, slash - 1);
                        bool aliasOk = !a.empty() && std::all_of(a.begin(), a.end(), isAliasIdent);
                        if (aliasOk)
                        {
                            sourceAlias = std::move(a);
                            inner = Trim(inner.substr(slash + 1));
                        }
                    }
                }
                else
                {
                    // alias:path — but only if `:` isn't part of guid:/GUID:
                    // (guid prefix is stripped above) and the prefix is a
                    // valid identifier. Skip well-known reserved prefixes.
                    size_t colon = inner.find(':');
                    if (colon != std::string::npos && colon > 0)
                    {
                        std::string prefix = inner.substr(0, colon);
                        // Reserved schemes that downstream code already handles
                        // and must NOT be eaten as a source alias.
                        // - guid/GUID: GUID literal (handled below).
                        // - url:       optional explicit form, stripped above.
                        // - asset:     deferred-resolution asset URI, kept verbatim
                        //              in value (see CSSParserTests.ParsesBackgroundImageAssetScheme).
                        // - data/http/https/file: web-style URIs, not asset sources.
                        const bool reserved = (prefix == "guid" || prefix == "GUID" ||
                                               prefix == "url" || prefix == "asset" ||
                                               prefix == "data" || prefix == "file" ||
                                               prefix == "http" || prefix == "https");
                        const bool aliasOk =
                            !prefix.empty() &&
                            !reserved &&
                            std::all_of(prefix.begin(), prefix.end(), isAliasIdent);
                        if (aliasOk)
                        {
                            sourceAlias = std::move(prefix);
                            inner = Trim(inner.substr(colon + 1));
                        }
                    }
                }
                // The rooted spelling `alias:/path` means "root of that
                // source". Strip the slash at the split so the stored value
                // is source-relative on every platform — on POSIX a leading
                // '/' would otherwise read as a genuine absolute path and
                // bypass source resolution entirely. (The @alias/ form
                // consumes its separator in the split above.)
                if (!sourceAlias.empty())
                {
                    const size_t firstKept = inner.find_first_not_of('/');
                    inner = firstKept == std::string::npos ? std::string{}
                                                           : inner.substr(firstKept);
                }
            }
            // Accept guid: prefix (after alias detection, since guid is reserved above)
            if (inner.rfind("guid:", 0) == 0 || inner.rfind("GUID:", 0) == 0)
                inner = inner.substr(5);
            // Trim again
            inner = Trim(inner);
            try
            {
                GameEngine::GUID g{GameEngine::String(inner)};
                if (!g.IsNull())
                {
                    src.Kind = BackgroundImageSource::SourceKind::Guid;
                    src.Guid = g;
                    // sourceAlias on a Guid is meaningless (GUID is
                    // already global). Drop it to avoid misleading
                    // future readers.
                }
                else if (!inner.empty())
                {
                    // Not a GUID: treat as path relative to AssetRoot
                    src.Kind = BackgroundImageSource::SourceKind::Path;
                    src.Value = inner;
                    src.SourceAlias = std::move(sourceAlias);
                }
            }
            catch (...)
            {
                // Not a GUID: treat as path
                if (!inner.empty())
                {
                    src.Kind = BackgroundImageSource::SourceKind::Path;
                    src.Value = inner;
                    src.SourceAlias = std::move(sourceAlias);
                }
            }
        }
    }
    return src;
}

static StyleValue ParseBackgroundRepeatValue(const std::string& value)
{
    const std::string v = ToLowerAscii(Trim(value));
    // `space` and `round` tile the image with different gap/scale handling,
    // neither of which is modelled; they still tile, so they map to Repeat.
    if (v == "repeat" || v == "space" || v == "round")
        return BackgroundRepeat::Repeat;
    if (v == "repeat-x")
        return BackgroundRepeat::RepeatX;
    if (v == "repeat-y")
        return BackgroundRepeat::RepeatY;
    return BackgroundRepeat::NoRepeat;
}

static StyleValue ParseBackgroundSizeValue(const std::string& value)
{
    std::istringstream ss(Trim(value));
    std::string a, b;
    ss >> a;
    ss >> b;
    auto parseLen = [](const std::string& s, float& out, bool& isPct)
    {
        if (s.empty() || Ieq(s, "auto"))
        {
            out = -1.0f;
            isPct = false;
            return;
        }
        isPct = (!s.empty() && s.back() == '%');
        out = ParseFloatPx(s);
    };
    BackgroundSizeValue size{};
    const std::string al = ToLowerAscii(a);
    if (al == "cover")
    {
        size.Mode = BackgroundSizeMode::Cover;
    }
    else if (al == "contain")
    {
        size.Mode = BackgroundSizeMode::Contain;
    }
    else if (!a.empty())
    {
        size.Mode = BackgroundSizeMode::Explicit;
        parseLen(a, size.SizeX, size.SizeXIsPercent);
        if (!b.empty())
            parseLen(b, size.SizeY, size.SizeYIsPercent);
    }
    return size;
}

static StyleValue ParseBackgroundPositionValue(const std::string& value)
{
    std::istringstream ss(ToLowerAscii(Trim(value)));
    std::string xTok, yTok;
    ss >> xTok;
    ss >> yTok;
    BackgroundPositionValue pos{};
    auto setKeyword = [&](const std::string& t, bool isX)
    {
        if (t == "left")
        {
            if (isX)
            {
                pos.X = 0;
                pos.XIsPercent = true;
            }
        }
        else if (t == "center")
        {
            if (isX)
            {
                pos.X = 50;
                pos.XIsPercent = true;
            }
            else
            {
                pos.Y = 50;
                pos.YIsPercent = true;
            }
        }
        else if (t == "right")
        {
            if (isX)
            {
                pos.X = 100;
                pos.XIsPercent = true;
            }
        }
        else if (t == "top")
        {
            if (!isX)
            {
                pos.Y = 0;
                pos.YIsPercent = true;
            }
        }
        else if (t == "bottom")
        {
            if (!isX)
            {
                pos.Y = 100;
                pos.YIsPercent = true;
            }
        }
    };
    auto parseLen = [](const std::string& s, float& out, bool& isPct)
    {
        if (s.empty())
            return;
        isPct = (!s.empty() && s.back() == '%');
        out = ParseFloatPx(s);
    };
    if (!xTok.empty() && (xTok == "left" || xTok == "center" || xTok == "right"))
    {
        setKeyword(xTok, true);
        if (!yTok.empty())
        {
            if (yTok == "top" || yTok == "center" || yTok == "bottom")
                setKeyword(yTok, false);
            else
                parseLen(yTok, pos.Y, pos.YIsPercent);
        }
    }
    else if (!xTok.empty() && (xTok == "top" || xTok == "bottom"))
    {
        setKeyword(xTok, false);
        if (!yTok.empty())
        {
            if (yTok == "left" || yTok == "center" || yTok == "right")
                setKeyword(yTok, true);
            else
                parseLen(yTok, pos.X, pos.XIsPercent);
        }
    }
    else
    {
        if (!xTok.empty())
            parseLen(xTok, pos.X, pos.XIsPercent);
        if (!yTok.empty())
            parseLen(yTok, pos.Y, pos.YIsPercent);
    }
    return pos;
}

static StyleValue ParseBorderImageSliceValue(const std::string& value)
{
    // 1-4 unitless/px numbers (CSS shorthand: T | T LR | T LR B | T R B L,
    // source texels) plus optional `fill` keyword. `border-image-slice: 0`
    // overrides the texture's intrinsic slice to "no border" (stretched).
    BorderImageSliceValue s{};
    s.Fill = false;
    std::istringstream ss(ToLowerAscii(Trim(value)));
    std::string tok;
    float nums[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int nc = 0;
    while (ss >> tok)
    {
        if (tok == "fill")
            s.Fill = true;
        else if (nc < 4)
            nums[nc++] = ParseFloatPx(tok);
    }
    if (nc == 1) { s.Top = s.Right = s.Bottom = s.Left = nums[0]; }
    else if (nc == 2) { s.Top = s.Bottom = nums[0]; s.Right = s.Left = nums[1]; }
    else if (nc == 3) { s.Top = nums[0]; s.Right = s.Left = nums[1]; s.Bottom = nums[2]; }
    else if (nc >= 4) { s.Top = nums[0]; s.Right = nums[1]; s.Bottom = nums[2]; s.Left = nums[3]; }
    return s;
}

static StyleValue ParseBorderImageRepeatValue(const std::string& value)
{
    // 1-2 keywords (stretch | repeat | round | space). `space` maps to tile.
    std::istringstream ss(ToLowerAscii(Trim(value)));
    std::string a, b;
    ss >> a;
    ss >> b;
    auto toMode = [](const std::string& s) {
        if (s == "repeat" || s == "space") return NineSliceFill::Tile;
        if (s == "round") return NineSliceFill::Round;
        return NineSliceFill::Stretch;
    };
    BorderImageRepeatValue r{};
    r.X = toMode(a);
    r.Y = b.empty() ? r.X : toMode(b);
    return r;
}

static StyleValue ParseBoxShadowValue(const std::string& value)
{
    if (ToLowerAscii(Trim(value)) == "none")
        return BoxShadowValue{};
    const auto toks = TokenizeWhitespaceOutsideParens(value);
    BoxShadowValue sv{};
    std::vector<std::string> rest;
    rest.reserve(toks.size());
    for (const auto& tok : toks)
    {
        if (ToLowerAscii(tok) == "inset")
            sv.Inset = true;
        else
            rest.push_back(tok);
    }
    if (rest.size() >= 1) sv.OffsetX = ParseFloatPx(rest[0]);
    if (rest.size() >= 2) sv.OffsetY = ParseFloatPx(rest[1]);
    if (rest.size() >= 3) sv.Blur = std::max(0.0f, ParseFloatPx(rest[2]));
    if (rest.size() >= 4) sv.Color = ParseColor(Trim(rest[3]));
    return sv;
}

static StyleValue ParseGlowValue(const std::string& value)
{
    if (ToLowerAscii(Trim(value)) == "none")
        return GlowValue{};
    const auto toks = TokenizeWhitespaceOutsideParens(value);
    GlowValue gv{};
    if (toks.size() >= 1) gv.Radius = std::max(0.0f, ParseFloatPx(toks[0]));
    if (toks.size() >= 2) gv.Color = ParseColor(Trim(toks[1]));
    return gv;
}

static void AppendTextEffect(StylePropertyId id, StyleValue value,
                             std::vector<StyleProperty>& out)
{
    StyleProperty property{};
    property.PropertyId = id;
    property.Value = std::move(value);
    out.push_back(std::move(property));
}

// A text effect value that does not parse is ignored as CSS requires, with a
// warning that states the accepted form, once per distinct declaration: a
// var() declaration is expanded again on every style compute.
static void WarnIgnoredTextEffect(std::string_view property, const std::string& value,
                                  std::string_view form)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> warned;
    std::string declaration = std::string(property) + ": " + Trim(value);
    {
        std::lock_guard lock(mutex);
        if (!warned.insert(declaration).second)
            return;
    }
    Logger::Log::Warning("UI CSS: '{}' is ignored. Write '{}: {}': every length in px (em is not accepted) and the color written out, since it does not default to the text color.",
                         declaration, property, form);
}

// <offset-x> <offset-y> [<blur>] with a <color> before or after the lengths.
static bool ParseTextShadow(const std::string& value, BoxShadowValue& shadow)
{
    auto tokens = TokenizeWhitespaceOutsideParens(value);
    // Blur is optional for a sharp shadow; sharing the box parser misread its
    // color as a length.
    if (tokens.size() < 3 || tokens.size() > 4) return false;
    bool numeric = false;
    ParseFloatPx(tokens.front(), &numeric);
    const auto colorIndex = numeric ? tokens.size() - 1 : size_t{0};
    const auto color = TryParseColorValue(tokens[colorIndex]);
    if (!color) return false;
    tokens.erase(tokens.begin() + colorIndex);
    std::array<float, 3> lengths{};
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const auto& token = tokens[i];
        char* end = nullptr;
        const float length = std::strtof(token.c_str(), &end);
        if (end == token.c_str() || !std::isfinite(length)) return false;
        const std::string suffix = ToLowerAscii(Trim(end));
        if (!suffix.empty() && suffix != "px") return false;
        lengths[i] = length;
    }
    if (lengths[2] < 0) return false;
    shadow.OffsetX = lengths[0];
    shadow.OffsetY = lengths[1];
    shadow.Blur = lengths[2];
    shadow.Color = *color;
    return true;
}

static void ExpandTextShadow(const std::string& value, std::vector<StyleProperty>& out)
{
    BoxShadowValue shadow{};
    if (ToLowerAscii(Trim(value)) != "none" && !ParseTextShadow(value, shadow))
    {
        WarnIgnoredTextEffect("text-shadow", value, "<x>px <y>px [<blur>px] <color>");
        return;
    }
    AppendTextEffect(StylePropertyId::TextShadowOffsetX, shadow.OffsetX, out);
    AppendTextEffect(StylePropertyId::TextShadowOffsetY, shadow.OffsetY, out);
    AppendTextEffect(StylePropertyId::TextShadowBlur, shadow.Blur, out);
    AppendTextEffect(StylePropertyId::TextShadowColor, shadow.Color, out);
}

// <length> <color>, the length not negative.
static bool ParseTextRadiusEffect(const std::string& value, float& radius, uint32_t& color)
{
    const auto tokens = TokenizeWhitespaceOutsideParens(value);
    if (tokens.size() != 2) return false;
    char* end = nullptr;
    radius = std::strtof(tokens[0].c_str(), &end);
    if (end == tokens[0].c_str() || !std::isfinite(radius) || radius < 0) return false;
    const auto suffix = ToLowerAscii(Trim(end));
    if (!suffix.empty() && suffix != "px") return false;
    const auto parsedColor = TryParseColorValue(tokens[1]);
    if (!parsedColor) return false;
    color = *parsedColor;
    return true;
}

static void ExpandTextRadiusEffect(const std::string& value, std::string_view property,
                                   std::string_view form, StylePropertyId radiusId,
                                   StylePropertyId colorId, std::vector<StyleProperty>& out)
{
    float radius = 0.0f;
    uint32_t color = 0;
    if (ToLowerAscii(Trim(value)) != "none" && !ParseTextRadiusEffect(value, radius, color))
    {
        WarnIgnoredTextEffect(property, value, form);
        return;
    }
    AppendTextEffect(radiusId, radius, out);
    AppendTextEffect(colorId, color, out);
}

static void ExpandTextGlow(const std::string& value, std::vector<StyleProperty>& out)
{
    ExpandTextRadiusEffect(value, "text-glow", "<radius>px <color>", StylePropertyId::TextGlowRadius,
                           StylePropertyId::TextGlowColor, out);
}

static void ExpandTextOutline(const std::string& value, std::vector<StyleProperty>& out)
{
    ExpandTextRadiusEffect(value, "text-outline", "<width>px <color>", StylePropertyId::TextOutlineWidth,
                           StylePropertyId::TextOutlineColor, out);
}

static StyleValue ParseTransitionValue(const std::string& value)
{
    std::vector<TransitionEntry> entries;
    if (ToLowerAscii(Trim(value)) != "none")
    {
        // Split by comma for multiple transitions
        std::vector<std::string> segments;
        {
            std::string cur;
            for (char c : value)
            {
                if (c == ',')
                {
                    std::string t = Trim(cur);
                    if (!t.empty())
                        segments.push_back(std::move(t));
                    cur.clear();
                }
                else
                {
                    cur.push_back(c);
                }
            }
            std::string t = Trim(cur);
            if (!t.empty())
                segments.push_back(std::move(t));
        }

        for (const std::string& seg : segments)
        {
            std::istringstream ss(seg);
            std::string tok;
            std::vector<std::string> toks;
            while (ss >> tok)
                toks.push_back(tok);
            if (toks.empty())
                continue;

            TransitionEntry entry{};
            size_t idx = 0;

            // "all" and unrecognized names (including pure shorthands like
            // "border") transition all properties.
            const std::string propName = ToLowerAscii(toks[idx]);
            const PropertyRow* propRow =
                (propName == "all") ? nullptr : FindPropertyRow(propName);
            entry.Property = propRow ? propRow->Id : StylePropertyId::Unknown;
            ++idx;

            bool haveDuration = false;
            for (; idx < toks.size(); ++idx)
            {
                bool timeOk = false;
                float timeSec = ParseTimeSec(toks[idx], &timeOk);
                if (timeOk)
                {
                    if (!haveDuration)
                    {
                        entry.DurationSec = std::max(0.0f, timeSec);
                        haveDuration = true;
                    }
                    else
                    {
                        entry.DelaySec = timeSec;
                    }
                }
                else if (IsEasingKeyword(toks[idx]))
                {
                    entry.Easing = ParseEasingKeyword(toks[idx]);
                }
            }

            entries.push_back(entry);
        }
    }
    return entries;
}

// --- shorthand expansions -------------------------------------------------
// CSS-wide keywords (inherit/initial/unset) never reach these: the driver
// fans keywords out to KeywordTargets before Expand runs.

// One scan of a `border` / `border-<side>` value: <width> || <style> || <color>
// in any order. Each Have* flag says whether the declaration NAMED that
// component; components it omits are left for the caller to skip rather than
// reset to their CSS initial values, which is what the box shorthand has always
// done and what the per-side shorthands must keep doing to stay its equal.
struct BorderShorthandParts
{
    float Width = 0.0f;
    BorderStyle Style = BorderStyle::Solid;
    uint32_t Color = 0x000000FFu;
    bool HasWidth = false;
    bool HasStyle = false;
    bool HasColor = false;
};

static BorderShorthandParts ScanBorderShorthand(const std::string& value)
{
    BorderShorthandParts parts{};

    for (const std::string& tokRaw : TokenizeWhitespaceOutsideParens(value))
    {
        const std::string tok = Trim(tokRaw);
        if (tok.empty())
            continue;

        bool ok = false;
        const float f = ParseFloatPx(tok, &ok);
        if (ok)
        {
            parts.Width = f;
            parts.HasWidth = true;
            continue;
        }

        const std::string tl = ToLowerAscii(tok);
        if (tl == "none" || tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "hidden" ||
            tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset")
        {
            parts.Style = std::get<BorderStyle>(ParseBorderStyleValue(tl));
            parts.HasStyle = true;
            continue;
        }

        // Treat remaining tokens as colors (named/rgb/hex/etc.)
        const uint32_t parsed = ParseColor(tok);
        // ParseColor uses 0xFFFFFFFF as a fallback; accept it for tokens that
        // look like colors (e.g. white/#fff/rgb(...)).
        const bool looksLikeColor =
            (tl == "transparent" || tl == "white" ||
             tok[0] == '#' ||
             (tl.rfind("rgb(", 0) == 0) || (tl.rfind("rgba(", 0) == 0) ||
             (tl.rfind("hsl(", 0) == 0) || (tl.rfind("hsla(", 0) == 0) ||
             (tl.size() > 2 && tl[0] == '0' && tl[1] == 'x') ||
             parsed != 0xFFFFFFFFu);
        if (looksLikeColor)
        {
            parts.Color = parsed;
            parts.HasColor = true;
        }
    }

    return parts;
}

// "border: <width> <style?> <color>"
static void ExpandBorderValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    const BorderShorthandParts parts = ScanBorderShorthand(value);

    if (parts.HasWidth)
        AppendBorderWidthLonghands(parts.Width, parts.Width, parts.Width, parts.Width, outProps);
    if (parts.HasColor)
        AppendBorderColorLonghands(parts.Color, parts.Color, parts.Color, parts.Color, outProps);
    if (parts.HasStyle)
    {
        StyleProperty p{};
        p.PropertyId = StylePropertyId::BorderStyle;
        p.Value = parts.Style;
        outProps.push_back(p);
    }
}

// "border-<side>: <width> || <style> || <color>" -> that side's width and
// colour longhands, and nothing else.
//
// `border-style` in this engine is a WHOLE-BOX property, so a per-side <style>
// has nowhere per-side to live. It is honoured for the one effect that IS
// expressible per side: `none` and `hidden` zero THIS side's width, which is
// the CSS used value and what `border-top: none` has to mean. A visible style
// keyword (solid/dashed/...) is accepted and deliberately writes nothing: one
// side must never restyle the other three.
//
// A suppressed side leaves its cascaded COLOUR alone rather than zeroing it,
// matching how the box shorthand treats a component the declaration omits.
// Nothing downstream reads it either way: DrawnBorderColor takes the colour of
// the first side with a non-zero WIDTH (UIManager_PrimitiveGen.cpp), so a side
// zeroed here is skipped whatever colour it holds.
//
// Two consequences, both intended and both far narrower than dropping the
// declaration outright: a per-side `dashed`/`dotted` paints solid, and a
// per-side width on a box whose border-style is `none` stays invisible, because
// ResolveUsedBorderWidths zeroes all four widths there.
static void ExpandBorderSideValue(const std::string& value, StylePropertyId widthId,
                                  StylePropertyId colorId, std::vector<StyleProperty>& outProps)
{
    const BorderShorthandParts parts = ScanBorderShorthand(value);
    const bool suppressed = parts.HasStyle && parts.Style == BorderStyle::None;

    if (suppressed || parts.HasWidth)
    {
        StyleProperty w{};
        w.PropertyId = widthId;
        w.Value = suppressed ? 0.0f : parts.Width;
        outProps.push_back(w);
    }
    if (!suppressed && parts.HasColor)
    {
        StyleProperty c{};
        c.PropertyId = colorId;
        c.Value = parts.Color;
        outProps.push_back(c);
    }
}

static void ExpandBorderTopValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    ExpandBorderSideValue(value, StylePropertyId::BorderTopWidth, StylePropertyId::BorderTopColor, outProps);
}

static void ExpandBorderRightValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    ExpandBorderSideValue(value, StylePropertyId::BorderRightWidth, StylePropertyId::BorderRightColor, outProps);
}

static void ExpandBorderBottomValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    ExpandBorderSideValue(value, StylePropertyId::BorderBottomWidth, StylePropertyId::BorderBottomColor, outProps);
}

static void ExpandBorderLeftValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    ExpandBorderSideValue(value, StylePropertyId::BorderLeftWidth, StylePropertyId::BorderLeftColor, outProps);
}

// outline: <width> || <style> || <color>, in any order.
//
// Width and style are always emitted, at their CSS initial values when the
// declaration omits them — that is what makes `outline: none` an opt-out
// rather than a style-only tweak that leaves an earlier width in place.
// Colour is emitted only when named: the engine has no `currentColor` value
// to reset to, and an omitted colour already resolves to the element's
// `color` at paint time (VisualStyle::HasOutlineColor). The one shape where
// the difference would be observable — a rule that sets a colour and a later
// rule that re-declares the shorthand without one — leaves the earlier
// colour standing instead of falling back to currentColor.
static void ExpandOutlineValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    float width = kMediumLineWidthPx;
    BorderStyle style = BorderStyle::None;
    uint32_t color = 0u;
    bool haveColor = false;

    for (const std::string& tokRaw : TokenizeWhitespaceOutsideParens(value))
    {
        const std::string tok = Trim(tokRaw);
        if (tok.empty())
            continue;
        const std::string tl = ToLowerAscii(tok);

        if (tl == "thin" || tl == "medium" || tl == "thick")
        {
            width = std::get<float>(ParseLineWidthValue(tl));
            continue;
        }
        if (tl == "none" || tl == "hidden" || tl == "auto" || tl == "solid" || tl == "dashed" ||
            tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" ||
            tl == "outset")
        {
            style = std::get<BorderStyle>(ParseOutlineStyleValue(tl));
            continue;
        }

        bool ok = false;
        const float f = ParseFloatPx(tok, &ok);
        if (ok)
        {
            width = std::max(0.0f, f);
            continue;
        }

        // ParseColor answers opaque white for anything it fails on, so only
        // tokens that actually look like colours are taken as one; the rest
        // are dropped rather than turned into a white ring.
        const uint32_t parsed = ParseColor(tok);
        const bool looksLikeColor =
            (tl == "transparent" || tl == "white" || tok[0] == '#' ||
             tl.rfind("rgb(", 0) == 0 || tl.rfind("rgba(", 0) == 0 ||
             tl.rfind("hsl(", 0) == 0 || tl.rfind("hsla(", 0) == 0 ||
             (tl.size() > 2 && tl[0] == '0' && tl[1] == 'x') || parsed != 0xFFFFFFFFu);
        if (looksLikeColor)
        {
            color = parsed;
            haveColor = true;
        }
    }

    StyleProperty w{};
    w.PropertyId = StylePropertyId::OutlineWidth;
    w.Value = width;
    outProps.push_back(w);

    StyleProperty s{};
    s.PropertyId = StylePropertyId::OutlineStyle;
    s.Value = style;
    outProps.push_back(s);

    if (haveColor)
    {
        StyleProperty c{};
        c.PropertyId = StylePropertyId::OutlineColor;
        c.Value = color;
        outProps.push_back(c);
    }
}

// border-width: 1-4 values (top/right/bottom/left)
static void ExpandBorderWidthValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    const auto toks = TokenizeWhitespaceOutsideParens(value);
    if (toks.empty())
        return;

    std::vector<float> vals;
    vals.reserve(std::min<size_t>(4, toks.size()));
    for (size_t i = 0; i < toks.size() && i < 4; ++i)
    {
        bool ok = false;
        const float f = ParseFloatPx(toks[i], &ok);
        if (!ok)
            return;
        vals.push_back(f);
    }
    if (vals.empty())
        return;

    float t = 0, r = 0, b = 0, l = 0;
    if (vals.size() == 1)
    {
        t = r = b = l = vals[0];
    }
    else if (vals.size() == 2)
    {
        t = b = vals[0];
        r = l = vals[1];
    }
    else if (vals.size() == 3)
    {
        t = vals[0];
        r = l = vals[1];
        b = vals[2];
    }
    else
    {
        t = vals[0];
        r = vals[1];
        b = vals[2];
        l = vals[3];
    }
    AppendBorderWidthLonghands(t, r, b, l, outProps);
}

// border-color: 1-4 values (top/right/bottom/left)
static void ExpandBorderColorValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    const auto toks = TokenizeWhitespaceOutsideParens(value);
    if (toks.empty())
        return;

    std::vector<uint32_t> cols;
    cols.reserve(std::min<size_t>(4, toks.size()));
    for (size_t i = 0; i < toks.size() && i < 4; ++i)
        cols.push_back(ParseColor(Trim(toks[i])));

    uint32_t t = 0, r = 0, b = 0, l = 0;
    if (cols.size() == 1)
    {
        t = r = b = l = cols[0];
    }
    else if (cols.size() == 2)
    {
        t = b = cols[0];
        r = l = cols[1];
    }
    else if (cols.size() == 3)
    {
        t = cols[0];
        r = l = cols[1];
        b = cols[2];
    }
    else
    {
        t = cols[0];
        r = cols[1];
        b = cols[2];
        l = cols[3];
    }
    AppendBorderColorLonghands(t, r, b, l, outProps);
}

// border-radius: `<length-percentage>{1,4} [ / <length-percentage>{1,4} ]?`
// (css-backgrounds-3 §5.1). The values before the slash are the horizontal
// radii and the values after it the vertical ones; each side fans its 1-4
// values out to tl/tr/br/bl independently, and with no slash the vertical
// radii equal the horizontal (circular corners). A malformed side — empty,
// five values, or an unreadable component — rejects the whole declaration
// (CSS 2.1 §4.2). Components read through the same ParseRadiusToken the
// longhands use, so the unit sniff has one implementation.
static void ExpandBorderRadiusValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    std::string beforeSlash;
    std::string afterSlash;
    const bool hasSlash = SplitAtTopLevelSlash(value, beforeSlash, afterSlash);

    const auto parseSide = [](const std::string& side, std::array<StyleLength, 4>& out) {
        const auto toks = TokenizeWhitespaceOutsideParens(side);
        if (toks.empty() || toks.size() > 4)
            return false;
        std::array<StyleLength, 4> vals{};
        for (size_t i = 0; i < toks.size(); ++i)
        {
            if (!ParseRadiusToken(toks[i], vals[i]))
                return false;
        }
        switch (toks.size())
        {
        case 1:  out = {vals[0], vals[0], vals[0], vals[0]}; break;
        case 2:  out = {vals[0], vals[1], vals[0], vals[1]}; break;
        case 3:  out = {vals[0], vals[1], vals[2], vals[1]}; break;
        default: out = {vals[0], vals[1], vals[2], vals[3]}; break;
        }
        return true;
    };

    std::array<StyleLength, 4> horizontal{};
    if (!parseSide(beforeSlash, horizontal))
        return;
    std::array<StyleLength, 4> vertical = horizontal;
    if (hasSlash && !parseSide(afterSlash, vertical))
        return;

    AppendBorderRadiusLonghands({horizontal[0], vertical[0]}, {horizontal[1], vertical[1]},
                                {horizontal[2], vertical[2]}, {horizontal[3], vertical[3]},
                                outProps);
}

// One component of the `flex` shorthand.
enum class FlexComponent
{
    Invalid,
    Number, // bare <number>: a grow/shrink factor, or a basis when it is exactly 0
    Length, // <length>, <percentage> or `auto`: a basis
};

// A unit suffix is what separates a basis from a factor: `100px` is a basis,
// `100` is a grow factor. Unitless zero classifies as Number because it is
// *both* a valid <number> and a valid <length>; position decides which.
static FlexComponent ClassifyFlexComponent(const std::string& tok, float& outNumber, StyleLength& outBasis)
{
    if (Ieq(tok, "auto"))
    {
        outBasis = StyleLength::Auto();
        return FlexComponent::Length;
    }

    const char* begin = tok.c_str();
    char* end = nullptr;
    const float f = std::strtof(begin, &end);
    if (end == begin)
        return FlexComponent::Invalid; // not numeric, and not a basis keyword we can represent

    const std::string suffix = Trim(std::string(end));
    if (suffix.empty())
    {
        outNumber = f;
        return FlexComponent::Number;
    }

    if (f < 0.0f)
        return FlexComponent::Invalid; // negative flex-basis is invalid

    if (suffix == "%")
    {
        outBasis = StyleLength::Percent(f);
        return FlexComponent::Length;
    }

    // Any other unit is taken as px, which is how ParseLengthValue treats unit
    // suffixes for every other length property in this engine.
    outBasis = StyleLength::Px(f);
    return FlexComponent::Length;
}

// flex: none | [ <'flex-grow'> <'flex-shrink'>? || <'flex-basis'> ]  (css-flexbox-1 7.1.1)
//
// The two factors form a single adjacent, ordered group; the basis may sit before or
// after it. A component carrying a unit is always the basis, never a grow factor —
// `flex: 100px` is `1 1 100px`. Unitless zero is also a valid <length>, so a bare `0`
// left over once the factor group is closed becomes the basis (`flex: 2 3 0`).
//
// Omitted factors default to 1, an omitted basis to 0. A malformed declaration emits
// nothing so the cascaded value survives (CSS 2.1 4.2), the same shape
// ExpandBorderRadiusValue uses for a bad token list.
static void ExpandFlexValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    auto push = [&outProps](StylePropertyId id, StyleValue v)
    {
        StyleProperty p{};
        p.PropertyId = id;
        p.Value = std::move(v);
        outProps.push_back(std::move(p));
    };

    const std::string v = Trim(value);
    if (Ieq(v, "none"))
    {
        push(StylePropertyId::FlexGrow, 0.0f);
        push(StylePropertyId::FlexShrink, 0.0f);
        push(StylePropertyId::FlexBasis, StyleLength::Auto());
        return;
    }

    const auto toks = TokenizeWhitespaceOutsideParens(v);
    if (toks.empty() || toks.size() > 3)
        return;

    float grow = 1.0f;
    float shrink = 1.0f;
    StyleLength basis = StyleLength::Px(0.0f);
    bool haveFactors = false;
    bool haveBasis = false;

    for (size_t i = 0; i < toks.size();)
    {
        float number = 0.0f;
        StyleLength length{};
        const FlexComponent kind = ClassifyFlexComponent(toks[i], number, length);

        if (kind == FlexComponent::Length)
        {
            if (haveBasis)
                return; // two bases
            basis = length;
            haveBasis = true;
            ++i;
            continue;
        }

        if (kind == FlexComponent::Number && !haveFactors)
        {
            if (number < 0.0f)
                return; // negative flex-grow
            grow = number;
            haveFactors = true;
            ++i;

            // flex-shrink binds only immediately after flex-grow.
            float shrinkNumber = 0.0f;
            StyleLength unused{};
            if (i < toks.size() &&
                ClassifyFlexComponent(toks[i], shrinkNumber, unused) == FlexComponent::Number)
            {
                if (shrinkNumber < 0.0f)
                    return; // negative flex-shrink
                shrink = shrinkNumber;
                ++i;
            }
            continue;
        }

        // A number outside the factor group can only be a basis, and unitless zero
        // is the only number that is also a valid <length>.
        if (kind == FlexComponent::Number && !haveBasis && number == 0.0f)
        {
            basis = StyleLength::Px(0.0f);
            haveBasis = true;
            ++i;
            continue;
        }

        return; // invalid component, a second basis, or a third factor
    }

    if (!haveFactors && !haveBasis)
        return;

    push(StylePropertyId::FlexGrow, grow);
    push(StylePropertyId::FlexShrink, shrink);
    push(StylePropertyId::FlexBasis, basis);
}

// gap: `<row-gap> <column-gap>?` (css-align-3 §8) — one value reaches both
// gutters, two set the row gutter then the column gutter, each keeping its own
// unit. A malformed token, or a third one, rejects the declaration whole so the
// cascaded value stands (CSS 2.1 §4.2) — emitting nothing is how an expander
// rejects (same convention as ExpandBorderWidthValue).
static void ExpandGapValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    const auto toks = TokenizeWhitespaceOutsideParens(value);
    if (toks.empty() || toks.size() > 2)
        return;

    StyleLength row{};
    if (!ParseGapToken(toks[0], row))
        return;

    if (toks.size() == 1)
    {
        // The one-value form travels as the Gap id, whose appliers fan it to
        // both gutters — and which transitions watch directly.
        StyleProperty p{};
        p.PropertyId = StylePropertyId::Gap;
        p.Value = row;
        outProps.push_back(p);
        return;
    }

    StyleLength column{};
    if (!ParseGapToken(toks[1], column))
        return;

    StyleProperty r{};
    r.PropertyId = StylePropertyId::RowGap;
    r.Value = row;
    outProps.push_back(r);

    StyleProperty c{};
    c.PropertyId = StylePropertyId::ColumnGap;
    c.Value = column;
    outProps.push_back(c);
}

// overflow: the shorthand row plus per-axis rows (which accept a narrower
// keyword set — see ParseOverflowAxisValue).
static void ExpandOverflowValue(const std::string& value, std::vector<StyleProperty>& outProps)
{
    StyleProperty po{};
    po.PropertyId = StylePropertyId::Overflow;
    po.Value = ParseOverflowValue(value);
    outProps.push_back(po);

    const StyleValue perAxis = ParseOverflowAxisValue(value);
    StyleProperty px{};
    px.PropertyId = StylePropertyId::OverflowX;
    px.Value = perAxis;
    outProps.push_back(px);
    StyleProperty py{};
    py.PropertyId = StylePropertyId::OverflowY;
    py.Value = perAxis;
    outProps.push_back(py);
}

// --- the table --------------------------------------------------------------

constexpr StylePropertyId kBorderKeywordTargets[] = {
    StylePropertyId::BorderTopWidth, StylePropertyId::BorderRightWidth,
    StylePropertyId::BorderBottomWidth, StylePropertyId::BorderLeftWidth,
    StylePropertyId::BorderTopColor, StylePropertyId::BorderRightColor,
    StylePropertyId::BorderBottomColor, StylePropertyId::BorderLeftColor,
    StylePropertyId::BorderStyle,
};
// Per-side shorthands fan a CSS-wide keyword to their own two longhands only.
// BorderStyle is absent by the same rule that keeps it out of the expansion.
constexpr StylePropertyId kBorderTopKeywordTargets[] = {
    StylePropertyId::BorderTopWidth, StylePropertyId::BorderTopColor,
};
constexpr StylePropertyId kBorderRightKeywordTargets[] = {
    StylePropertyId::BorderRightWidth, StylePropertyId::BorderRightColor,
};
constexpr StylePropertyId kBorderBottomKeywordTargets[] = {
    StylePropertyId::BorderBottomWidth, StylePropertyId::BorderBottomColor,
};
constexpr StylePropertyId kBorderLeftKeywordTargets[] = {
    StylePropertyId::BorderLeftWidth, StylePropertyId::BorderLeftColor,
};
constexpr StylePropertyId kBorderWidthKeywordTargets[] = {
    StylePropertyId::BorderTopWidth, StylePropertyId::BorderRightWidth,
    StylePropertyId::BorderBottomWidth, StylePropertyId::BorderLeftWidth,
};
constexpr StylePropertyId kBorderColorKeywordTargets[] = {
    StylePropertyId::BorderTopColor, StylePropertyId::BorderRightColor,
    StylePropertyId::BorderBottomColor, StylePropertyId::BorderLeftColor,
};
constexpr StylePropertyId kBorderRadiusKeywordTargets[] = {
    StylePropertyId::BorderTopLeftRadius, StylePropertyId::BorderTopRightRadius,
    StylePropertyId::BorderBottomRightRadius, StylePropertyId::BorderBottomLeftRadius,
};
constexpr StylePropertyId kFlexKeywordTargets[] = {
    StylePropertyId::FlexGrow, StylePropertyId::FlexShrink, StylePropertyId::FlexBasis,
};
constexpr StylePropertyId kOverflowKeywordTargets[] = {
    StylePropertyId::Overflow, StylePropertyId::OverflowX, StylePropertyId::OverflowY,
};
constexpr StylePropertyId kOutlineKeywordTargets[] = {
    StylePropertyId::OutlineWidth, StylePropertyId::OutlineStyle, StylePropertyId::OutlineColor,
};

constexpr StylePropertyId kTextShadowTargets[] = {
    StylePropertyId::TextShadowOffsetX, StylePropertyId::TextShadowOffsetY,
    StylePropertyId::TextShadowBlur, StylePropertyId::TextShadowColor,
};
constexpr StylePropertyId kTextGlowTargets[] = {
    StylePropertyId::TextGlowRadius, StylePropertyId::TextGlowColor,
};
constexpr StylePropertyId kTextOutlineTargets[] = {
    StylePropertyId::TextOutlineWidth, StylePropertyId::TextOutlineColor,
};

constexpr PropertyRow kPropertyRows[] = {
    {"display", StylePropertyId::Display, &ParseDisplayValue},
    {"visibility", StylePropertyId::Visibility, &ParseVisibilityValue},
    {"pointer-events", StylePropertyId::PointerEvents, &ParsePointerEventsValue},
    {"opacity", StylePropertyId::Opacity, &ParseFloat01Value},
    {"overflow", StylePropertyId::Overflow, &ParseOverflowValue, &ExpandOverflowValue, kOverflowKeywordTargets},
    {"overflow-x", StylePropertyId::OverflowX, &ParseOverflowAxisValue},
    {"overflow-y", StylePropertyId::OverflowY, &ParseOverflowAxisValue},
    {"cursor", StylePropertyId::Cursor, &ParseCursorValue},
    {"text-align", StylePropertyId::TextAlign, &ParseTextAlignValue},
    {"word-break", StylePropertyId::WordBreak, &ParseWordBreakValue},
    {"overflow-wrap", StylePropertyId::OverflowWrap, &ParseOverflowWrapValue},
    // css-text-3 5.4: word-wrap is the legacy alias of overflow-wrap.
    {"word-wrap", StylePropertyId::OverflowWrap, &ParseOverflowWrapValue},
    {"white-space", StylePropertyId::WhiteSpace, &ParseWhiteSpaceValue},
    {"text-overflow", StylePropertyId::TextOverflow, &ParseTextOverflowValue},
    {"font-size", StylePropertyId::FontSize, &ParseFontSizeValue},
    {"line-height", StylePropertyId::LineHeight, &ParseLineHeightValue},
    {"letter-spacing", StylePropertyId::LetterSpacing, &ParseLetterSpacingValue},
    {"font-family", StylePropertyId::FontFamily, &ParseFontFamilyValue},
    {"font-weight", StylePropertyId::FontWeight, &ParseFontWeightValue},
    {"font-style", StylePropertyId::FontStyle, &ParseFontStyleValue},
    {"font-variant", StylePropertyId::FontVariant, &ParseFontVariantValue},
    {"color", StylePropertyId::Color, &ParseColorValue},
    {"background", StylePropertyId::BackgroundColor, &ParseColorValue},
    {"background-color", StylePropertyId::BackgroundColor, &ParseColorValue},
    {"background-image", StylePropertyId::BackgroundImage, &ParseBackgroundImageValue},
    {"background-repeat", StylePropertyId::BackgroundRepeat, &ParseBackgroundRepeatValue},
    {"background-size", StylePropertyId::BackgroundSize, &ParseBackgroundSizeValue},
    {"background-position", StylePropertyId::BackgroundPosition, &ParseBackgroundPositionValue},
    {"background-tint", StylePropertyId::BackgroundTint, &ParseColorValue},
    {"background-image-tint", StylePropertyId::BackgroundTint, &ParseColorValue},
    {"background-image-saturation", StylePropertyId::BackgroundImageSaturation, &ParseFloat01Value},
    {"border-image-slice", StylePropertyId::BorderImageSlice, &ParseBorderImageSliceValue},
    {"border-image-repeat", StylePropertyId::BorderImageRepeat, &ParseBorderImageRepeatValue},
    {"border", StylePropertyId::Unknown, nullptr, &ExpandBorderValue, kBorderKeywordTargets},
    {"border-top", StylePropertyId::Unknown, nullptr, &ExpandBorderTopValue, kBorderTopKeywordTargets},
    {"border-right", StylePropertyId::Unknown, nullptr, &ExpandBorderRightValue, kBorderRightKeywordTargets},
    {"border-bottom", StylePropertyId::Unknown, nullptr, &ExpandBorderBottomValue, kBorderBottomKeywordTargets},
    {"border-left", StylePropertyId::Unknown, nullptr, &ExpandBorderLeftValue, kBorderLeftKeywordTargets},
    {"border-color", StylePropertyId::BorderColor, nullptr, &ExpandBorderColorValue, kBorderColorKeywordTargets},
    {"border-top-color", StylePropertyId::BorderTopColor, &ParseColorValue},
    {"border-right-color", StylePropertyId::BorderRightColor, &ParseColorValue},
    {"border-bottom-color", StylePropertyId::BorderBottomColor, &ParseColorValue},
    {"border-left-color", StylePropertyId::BorderLeftColor, &ParseColorValue},
    {"border-width", StylePropertyId::BorderWidth, nullptr, &ExpandBorderWidthValue, kBorderWidthKeywordTargets},
    {"border-top-width", StylePropertyId::BorderTopWidth, &ParseFloatPxValue},
    {"border-right-width", StylePropertyId::BorderRightWidth, &ParseFloatPxValue},
    {"border-bottom-width", StylePropertyId::BorderBottomWidth, &ParseFloatPxValue},
    {"border-left-width", StylePropertyId::BorderLeftWidth, &ParseFloatPxValue},
    {"border-style", StylePropertyId::BorderStyle, &ParseBorderStyleValue},
    {"outline", StylePropertyId::Unknown, nullptr, &ExpandOutlineValue, kOutlineKeywordTargets},
    {"outline-width", StylePropertyId::OutlineWidth, &ParseLineWidthValue},
    {"outline-style", StylePropertyId::OutlineStyle, &ParseOutlineStyleValue},
    {"outline-color", StylePropertyId::OutlineColor, &ParseColorValue},
    {"outline-offset", StylePropertyId::OutlineOffset, &ParseFloatPxValue},
    {"border-radius", StylePropertyId::BorderRadius, nullptr, &ExpandBorderRadiusValue, kBorderRadiusKeywordTargets},
    {"border-top-left-radius", StylePropertyId::BorderTopLeftRadius, &ParseRadiusLengthValue},
    {"border-top-right-radius", StylePropertyId::BorderTopRightRadius, &ParseRadiusLengthValue},
    {"border-bottom-right-radius", StylePropertyId::BorderBottomRightRadius, &ParseRadiusLengthValue},
    {"border-bottom-left-radius", StylePropertyId::BorderBottomLeftRadius, &ParseRadiusLengthValue},
    {"margin", StylePropertyId::Margin, &ParseBoxValue},
    {"margin-top", StylePropertyId::MarginTop, &ParseLengthValue},
    {"margin-right", StylePropertyId::MarginRight, &ParseLengthValue},
    {"margin-bottom", StylePropertyId::MarginBottom, &ParseLengthValue},
    {"margin-left", StylePropertyId::MarginLeft, &ParseLengthValue},
    {"padding", StylePropertyId::Padding, &ParseBoxValue},
    {"padding-top", StylePropertyId::PaddingTop, &ParseLengthValue},
    {"padding-right", StylePropertyId::PaddingRight, &ParseLengthValue},
    {"padding-bottom", StylePropertyId::PaddingBottom, &ParseLengthValue},
    {"padding-left", StylePropertyId::PaddingLeft, &ParseLengthValue},
    // No KeywordTargets: a CSS-wide keyword lands on the Gap id, whose
    // CopyPropertyFromStyle case carries both gutters, like the one-value form.
    {"gap", StylePropertyId::Gap, nullptr, &ExpandGapValue},
    {"row-gap", StylePropertyId::RowGap, &ParseGapLengthValue},
    {"column-gap", StylePropertyId::ColumnGap, &ParseGapLengthValue},
    {"width", StylePropertyId::Width, &ParseLengthValue},
    {"height", StylePropertyId::Height, &ParseLengthValue},
    {"min-width", StylePropertyId::MinWidth, &ParseLengthValue},
    {"min-height", StylePropertyId::MinHeight, &ParseLengthValue},
    {"max-width", StylePropertyId::MaxWidth, &ParseMaxLengthValue},
    {"max-height", StylePropertyId::MaxHeight, &ParseMaxLengthValue},
    {"aspect-ratio", StylePropertyId::AspectRatio, &ParseAspectRatioValue},
    {"flex", StylePropertyId::Unknown, nullptr, &ExpandFlexValue, kFlexKeywordTargets},
    {"flex-direction", StylePropertyId::FlexDir, &ParseFlexDirectionValue},
    {"flex-wrap", StylePropertyId::FlexWrap, &ParseFlexWrapValue},
    {"justify-content", StylePropertyId::JustifyContent, &ParseJustifyContentValue},
    {"align-items", StylePropertyId::AlignItems, &ParseAlignItemsValue},
    {"align-self", StylePropertyId::AlignSelf, &ParseAlignSelfValue},
    {"align-content", StylePropertyId::AlignContent, &ParseAlignContentValue},
    {"direction", StylePropertyId::Direction, &ParseDirectionValue},
    {"position", StylePropertyId::Position, &ParsePositionValue},
    {"left", StylePropertyId::PositionLeft, &ParseLengthValue},
    {"top", StylePropertyId::PositionTop, &ParseLengthValue},
    {"right", StylePropertyId::PositionRight, &ParseLengthValue},
    {"bottom", StylePropertyId::PositionBottom, &ParseLengthValue},
    {"order", StylePropertyId::Order, &ParseOrderValue},
    {"z-index", StylePropertyId::ZIndex, &ParseZIndexValue},
    {"flex-grow", StylePropertyId::FlexGrow, &ParseFloatPxValue},
    {"flex-shrink", StylePropertyId::FlexShrink, &ParseFloatPxValue},
    {"flex-basis", StylePropertyId::FlexBasis, &ParseLengthValue},
    {"box-shadow", StylePropertyId::BoxShadow, &ParseBoxShadowValue},
    {"text-shadow", StylePropertyId::Unknown, nullptr, &ExpandTextShadow, kTextShadowTargets},
    {"text-glow", StylePropertyId::Unknown, nullptr, &ExpandTextGlow, kTextGlowTargets},
    {"text-outline", StylePropertyId::Unknown, nullptr, &ExpandTextOutline, kTextOutlineTargets},
    {"text-shadow-offset-x", StylePropertyId::TextShadowOffsetX, &ParseFloatPxValue},
    {"text-shadow-offset-y", StylePropertyId::TextShadowOffsetY, &ParseFloatPxValue},
    {"text-shadow-blur", StylePropertyId::TextShadowBlur, &ParseFloatPxValue},
    {"text-shadow-color", StylePropertyId::TextShadowColor, &ParseColorValue},
    {"text-glow-radius", StylePropertyId::TextGlowRadius, &ParseFloatPxValue},
    {"text-glow-color", StylePropertyId::TextGlowColor, &ParseColorValue},
    {"text-outline-width", StylePropertyId::TextOutlineWidth, &ParseFloatPxValue},
    {"text-outline-color", StylePropertyId::TextOutlineColor, &ParseColorValue},
    {"selection-color", StylePropertyId::SelectionColor, &ParseColorValue},
    {"glow", StylePropertyId::Glow, &ParseGlowValue},
    {"transition", StylePropertyId::Transition, &ParseTransitionValue},
};

// --- compile-time table validation -------------------------------------------

constexpr char ToLowerAsciiChar(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool RowNamesAreLowercaseAndUnique()
{
    for (size_t i = 0; i < std::size(kPropertyRows); ++i)
    {
        for (char c : kPropertyRows[i].Name)
            if (c != ToLowerAsciiChar(c))
                return false;
        for (size_t j = i + 1; j < std::size(kPropertyRows); ++j)
            if (kPropertyRows[i].Name == kPropertyRows[j].Name)
                return false;
    }
    return true;
}

constexpr bool EveryRowHasAParserOrExpansion()
{
    for (const PropertyRow& row : kPropertyRows)
        if (row.Parse == nullptr && row.Expand == nullptr)
            return false;
    return true;
}

constexpr bool EveryPropertyIdHasARow()
{
    for (uint16_t i = 1; i < static_cast<uint16_t>(StylePropertyId::_Count); ++i)
    {
        const auto id = static_cast<StylePropertyId>(i);
        // Not declaration names: CustomVar comes from the --var branch,
        // DeferredDecl from the var() capture in the stylesheet parser.
        if (id == StylePropertyId::CustomVar || id == StylePropertyId::DeferredDecl)
            continue;
        bool found = false;
        for (const PropertyRow& row : kPropertyRows)
        {
            if (row.Id == id)
            {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static_assert(RowNamesAreLowercaseAndUnique(),
              "kPropertyRows names must be lowercase and unique (lookup lowercases the query)");
static_assert(EveryRowHasAParserOrExpansion(),
              "every kPropertyRows entry needs a Parse or Expand function");
static_assert(EveryPropertyIdHasARow(),
              "every StylePropertyId needs a kPropertyRows entry (or an explicit exclusion in EveryPropertyIdHasARow)");

static const PropertyRow* FindPropertyRow(const std::string& name)
{
    static const std::unordered_map<std::string_view, const PropertyRow*> index = []
    {
        std::unordered_map<std::string_view, const PropertyRow*> map;
        map.reserve(std::size(kPropertyRows));
        for (const PropertyRow& row : kPropertyRows)
            map.emplace(row.Name, &row);
        return map;
    }();
    const std::string lowered = ToLowerAscii(name);
    const auto it = index.find(std::string_view{lowered});
    return it == index.end() ? nullptr : it->second;
}

static std::vector<std::string> TokenizeWhitespaceOutsideParens(const std::string& value)
{
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(value.size());

    int parenDepth = 0;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;

    auto flush = [&]()
    {
        std::string t = Trim(cur);
        if (!t.empty())
            out.push_back(std::move(t));
        cur.clear();
    };

    for (char c : value)
    {
        if (c == '"' && !inSingleQuote)
        {
            inDoubleQuote = !inDoubleQuote;
            cur.push_back(c);
            continue;
        }
        if (c == '\'' && !inDoubleQuote)
        {
            inSingleQuote = !inSingleQuote;
            cur.push_back(c);
            continue;
        }

        if (!inSingleQuote && !inDoubleQuote)
        {
            if (c == '(')
            {
                parenDepth++;
                cur.push_back(c);
                continue;
            }
            if (c == ')')
            {
                if (parenDepth > 0)
                    parenDepth--;
                cur.push_back(c);
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)) && parenDepth == 0)
            {
                if (!cur.empty())
                    flush();
                continue;
            }
        }

        cur.push_back(c);
    }
    if (!cur.empty())
        flush();
    return out;
}

// Split at the first `/` outside parens and quotes. Returns whether one was
// found; `before` is always the (trimmed) part in front of it — the whole
// value when there is none — and `after` the (trimmed) part behind it.
static bool SplitAtTopLevelSlash(const std::string& value, std::string& before, std::string& after)
{
    int parenDepth = 0;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (c == '"' && !inSingleQuote)
            inDoubleQuote = !inDoubleQuote;
        else if (c == '\'' && !inDoubleQuote)
            inSingleQuote = !inSingleQuote;

        if (!inSingleQuote && !inDoubleQuote)
        {
            if (c == '(')
                parenDepth++;
            else if (c == ')')
                parenDepth = std::max(0, parenDepth - 1);
            else if (c == '/' && parenDepth == 0)
            {
                before = Trim(value.substr(0, i));
                after = Trim(value.substr(i + 1));
                return true;
            }
        }
    }
    before = Trim(value);
    after.clear();
    return false;
}

static void AppendBorderWidthLonghands(float top, float right, float bottom, float left, std::vector<StyleProperty>& outProps)
{
    StyleProperty pT{};
    pT.PropertyId = StylePropertyId::BorderTopWidth;
    pT.Value = top;
    outProps.push_back(pT);

    StyleProperty pR{};
    pR.PropertyId = StylePropertyId::BorderRightWidth;
    pR.Value = right;
    outProps.push_back(pR);

    StyleProperty pB{};
    pB.PropertyId = StylePropertyId::BorderBottomWidth;
    pB.Value = bottom;
    outProps.push_back(pB);

    StyleProperty pL{};
    pL.PropertyId = StylePropertyId::BorderLeftWidth;
    pL.Value = left;
    outProps.push_back(pL);
}

static void AppendBorderColorLonghands(uint32_t top, uint32_t right, uint32_t bottom, uint32_t left, std::vector<StyleProperty>& outProps)
{
    StyleProperty pT{};
    pT.PropertyId = StylePropertyId::BorderTopColor;
    pT.Value = top;
    outProps.push_back(pT);

    StyleProperty pR{};
    pR.PropertyId = StylePropertyId::BorderRightColor;
    pR.Value = right;
    outProps.push_back(pR);

    StyleProperty pB{};
    pB.PropertyId = StylePropertyId::BorderBottomColor;
    pB.Value = bottom;
    outProps.push_back(pB);

    StyleProperty pL{};
    pL.PropertyId = StylePropertyId::BorderLeftColor;
    pL.Value = left;
    outProps.push_back(pL);
}

static void AppendBorderRadiusLonghands(const CornerRadiusValue& tl, const CornerRadiusValue& tr, const CornerRadiusValue& br, const CornerRadiusValue& bl, std::vector<StyleProperty>& outProps)
{
    StyleProperty pTL{};
    pTL.PropertyId = StylePropertyId::BorderTopLeftRadius;
    pTL.Value = tl;
    outProps.push_back(pTL);

    StyleProperty pTR{};
    pTR.PropertyId = StylePropertyId::BorderTopRightRadius;
    pTR.Value = tr;
    outProps.push_back(pTR);

    StyleProperty pBR{};
    pBR.PropertyId = StylePropertyId::BorderBottomRightRadius;
    pBR.Value = br;
    outProps.push_back(pBR);

    StyleProperty pBL{};
    pBL.PropertyId = StylePropertyId::BorderBottomLeftRadius;
    pBL.Value = bl;
    outProps.push_back(pBL);
}

} // namespace

std::optional<uint32_t> CSSDetail::TryParseColor(const std::string& value)
{
    return TryParseColorValue(Trim(value));
}

StyleKeyword CSSDetail::ParseStyleKeyword(const std::string& rawValue)
{
    const std::string v = ToLowerAscii(Trim(rawValue));
    if (v == "inherit")
        return StyleKeyword::Inherit;
    if (v == "initial")
        return StyleKeyword::Initial;
    if (v == "unset")
        return StyleKeyword::Unset;
    return StyleKeyword::None;
}

void CSSDetail::AppendDeclarationProperties(const std::string& name, const std::string& value, std::vector<StyleProperty>& outProps)
{
    // Custom property --var-name: captured verbatim. The value only has
    // meaning at var() substitution time, so no keyword/value parsing here.
    if (name.size() >= 3 && name[0] == '-' && name[1] == '-')
    {
        StyleProperty p{};
        p.PropertyId = StylePropertyId::CustomVar;
        p.Value = CustomVarDecl{name, value};
        outProps.push_back(std::move(p));
        return;
    }

    const PropertyRow* row = FindPropertyRow(name);
    if (!row)
    {
        // Unknown property: emit an Unknown-id row so callers can see the
        // declaration was consumed (matches the pre-table parser).
        outProps.push_back(StyleProperty{});
        return;
    }

    // CSS-wide keywords resolve against parent/initial values at cascade
    // time; shorthands fan the keyword out to their longhand targets.
    const StyleKeyword kw = ParseStyleKeyword(value);
    if (kw != StyleKeyword::None)
    {
        const auto pushKeyword = [&outProps, kw](StylePropertyId id)
        {
            StyleProperty p{};
            p.PropertyId = id;
            p.Keyword = kw;
            outProps.push_back(p);
        };
        if (!row->KeywordTargets.empty())
        {
            for (const StylePropertyId id : row->KeywordTargets)
                pushKeyword(id);
        }
        else
        {
            pushKeyword(row->Id);
        }
        return;
    }

    if (row->Expand)
    {
        row->Expand(value, outProps);
        return;
    }

    StyleValue parsed = row->Parse(value);
    if (std::holds_alternative<std::monostate>(parsed))
    {
        // The parser rejected the value. CSS 2.1 4.2 requires the declaration
        // to be ignored so the cascaded value survives; emit the same
        // Unknown-id row an unrecognised property emits, which the cascade
        // switches skip, so callers still see the declaration was consumed.
        outProps.push_back(StyleProperty{});
        return;
    }

    StyleProperty p{};
    p.PropertyId = row->Id;
    p.Value = std::move(parsed);
    outProps.push_back(std::move(p));
}

StylePropertyImpact CSSParser::PropertyImpactForName(const std::string& name)
{
    const PropertyRow* row = FindPropertyRow(name);
    if (!row)
        return {};

    // A shorthand's impact is whatever its expansion writes. KeywordTargets is
    // that expansion set (it is what a CSS-wide keyword fans out to), so the
    // union over it answers for shorthands whose own Id is Unknown and
    // corrects the ones whose Id understates their expansion.
    StylePropertyImpact impact = GetStylePropertyImpact(row->Id);
    for (const StylePropertyId target : row->KeywordTargets)
    {
        const StylePropertyImpact targetImpact = GetStylePropertyImpact(target);
        impact.Layout = impact.Layout || targetImpact.Layout;
        impact.Paint = impact.Paint || targetImpact.Paint;
        impact.Subtree = impact.Subtree || targetImpact.Subtree;
    }
    return impact;
}

bool CSSParser::IsKnownPropertyName(const std::string& name)
{
    return FindPropertyRow(name) != nullptr;
}

} // namespace UIParsing
} // namespace GameEngine
