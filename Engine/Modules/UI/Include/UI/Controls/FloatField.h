#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

#include "Types/FormatNumber.h"
#include "UI/Controls/TextField.h"
#include "NumericExpression/NumericExpression.h"

namespace GameEngine {

// Display formatter (7 significant digits); defined with the other module-local
// formatting helpers below, forward-declared so OnPostLayout can pick it
// explicitly (its "full display text" is NOT the editing representation).
static std::string FormatFixedAdaptive(float v);

// Numeric field for editing a single float value. Supports math expressions
// (2+2, 3.5*2, sin(pi/2), sqrt(16), etc.) resolved on commit via TinyExpr.
// Display (unfocused) shows Unity-width 7 significant digits; when the text
// still overflows the field box it degrades to compact suffix notation
// (k/M/B/T / e-notation). Focus swaps to the shortest exact round-trip
// representation so editing never loses precision, and blur restores the
// display form with the stored value bit-exact throughout.
// Also accepts compact suffix input (e.g. "1.5k" = 1500, "2.3M" = 2300000).
class FloatField : public TextFieldBase<float>
{
public:
    using ValueType = float;

    FloatField();

    static bool TryParseFloat(const std::string& text, float& outValue);

    static void SetBigNumberSpacingEnabled(bool enabled) { m_BigNumberSpacingEnabled = enabled; }
    static bool IsBigNumberSpacingEnabled() { return m_BigNumberSpacingEnabled; }

    // Force a stable number of digits after the decimal point in both display
    // and edit modes. A negative value restores the adaptive formatter.
    void SetFixedDecimalPlaces(int places)
    {
        m_FixedDecimalPlaces = (places < 0) ? -1 : std::clamp(places, 0, 9);
        SetValueWithoutNotify(GetValue());
    }

    // Trim trailing zeros after the decimal point. "50.00" -> "50", "50.01" -> "50.01".
    // Public so the module-local formatting helpers below can reuse it.
    static std::string TrimTrailingZeros(std::string s);

    // Click-drag the value box to scrub; click without a drag to edit.
    // Matches the graph Variables panel numeric fields. Inspector floats
    // leave this off and scrub from their labels instead.
    void EnableDragToChange();
    bool IsDragToChangeEnabled() const { return m_DragToChangeEnabled; }

    // Clamp user edits (drag scrubs and typed input) to [min, max].
    // Fields are pooled and rebound; every bind must set or clear the range.
    void SetValueRange(float min, float max)
    {
        m_HasValueRange = true;
        m_ValueRangeMin = std::min(min, max);
        m_ValueRangeMax = std::max(min, max);
    }
    void ClearValueRange() { m_HasValueRange = false; }
    bool HasValueRange() const { return m_HasValueRange; }
    float ClampToValueRange(float value) const
    {
        return m_HasValueRange ? std::clamp(value, m_ValueRangeMin, m_ValueRangeMax) : value;
    }

protected:
    float ConstrainTypedValue(const float& value) const override { return ClampToValueRange(value); }

public:
    // Focus hook: switch between compact display and full precision.
    void OnFocusChanged(bool focused) override;
    bool OnChar(unsigned int codepoint) override;
    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override;
    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float w, float h,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override;
    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float w, float h,
                       const ResolvedStyle& style,
                       Rendering::Text::FontAtlas* font) override;
    void OnEvent(UIEvent& e) override;

    // After layout: check if the text overflows the field box and compact if needed.
    void OnPostLayout() override;

private:
    // Format the value. If compact is true, use suffix notation for large/small values.
    std::string FormatFloat(float v, bool compact);

    void BeginDragToChange(float mouseX);
    void UpdateDragToChange(float mouseX);
    void EndDragToChange();

    inline static bool m_BigNumberSpacingEnabled = true;
    bool m_UseCompactFormat = true;
    int m_FixedDecimalPlaces = -1;

    bool m_HasValueRange = false;
    float m_ValueRangeMin = 0.0f;
    float m_ValueRangeMax = 0.0f;

    bool m_DragToChangeEnabled = false;
    bool m_DragPending = false;
    bool m_DraggingValue = false;
    bool m_SuppressTextFocus = false;
    bool m_FocusableBeforeDrag = true;
    float m_DragStartX = 0.0f;
    float m_DragStartValue = 0.0f;
};

inline FloatField::FloatField()
{
    AddClass("float-field");
    // Control styling is an Editor-shipped UIStyle asset.
    RequestSubtreeStyleAssetPath("UI/controls/FloatField.css", "editor");
    ConfigureEditorIdentity("float-input-", "float-field-input");
    if (TextInput* editor = GetTextInput())
        editor->AddClass("numeric-field-input");

    SetFilterFunction([](const std::string& raw, bool /*isFinal*/)
    {
        return FilterNumericExpressionInput(raw);
    });

    SetParseFunction([](const std::string& textValue, float& out, bool /*isFinal*/)
    {
        return TryParseFloat(textValue, out);
    });

    // Capture 'this' so the format function can read m_UseCompactFormat.
    SetFormatFunction([this](float v)
    {
        return this->FormatFloat(v, this->m_UseCompactFormat);
    });

    SetEmptyValues(0.0f, 0.0f);
    SetValue(0.0f);
}

inline void FloatField::OnFocusChanged(bool focused)
{
    if (m_DragToChangeEnabled && focused && (m_DragPending || m_SuppressTextFocus))
        return;
    if (!focused)
        m_SuppressTextFocus = false;
    TextFieldBase<float>::OnFocusChanged(focused);

    // Switch formatting mode and reformat the current value.
    m_UseCompactFormat = !focused;
    this->SetValueWithoutNotify(this->GetValue());
}

// Produce a compact suffix representation at a given decimal precision.
// precision=2 -> "50.43B", precision=1 -> "50.4B", precision=0 -> "50B".
// Returns an empty string if the value is too small for suffix notation.
static std::string CompactSuffixAt(float v, int precision)
{
    const float av = std::abs(v);
    const bool negative = v < 0.0f;
    const float mag = negative ? av : v;
    if (mag < 1e3f)
        return "";  // too small for suffix

    const struct
    {
        float divisor;
        char suffix;
    } tiers[] = {
        {1e12f, 'T'},
        {1e9f, 'B'},
        {1e6f, 'M'},
        {1e3f, 'K'},
    };

    for (const auto& tier : tiers)
    {
        if (mag >= tier.divisor)
        {
            const std::string s = FormatFixed(mag / tier.divisor, precision);
            return (negative ? "-" : "") + s + tier.suffix;
        }
    }
    return "";
}

inline void FloatField::OnPostLayout()
{
    TextFieldBase<float>::OnPostLayout();

    // When focused we always show full precision; nothing to do here.
    if (!m_UseCompactFormat)
        return;

    TextInput* editor = GetTextInput();
    if (!editor)
        return;

    UIManager* owner = this->GetOwnerManager();
    if (!owner)
        return;

    // Get the font atlas for text measurement.
    Rendering::Text::FontAtlas* font = owner->GetDefaultFontAtlas();
    if (!font)
        return;

    // Measure against the embedded TextInput, not the outer FloatField. The
    // editor owns the rendered text box and can be narrower than the field
    // container, so using the outer width can choose text that clips the suffix.
    ResolvedStyle style = Field<float>::ComputePointerStyle(owner, editor);
    const float px = std::max(1.0f, style.Visual.FontSize);
    const float lsPx = style.Visual.LetterSpacing; // logical, like px here

    // Available inner width = TextInput layout width minus its horizontal padding.
    float W = editor->GetLayoutWidth();
    float padL = style.Layout.Padding.Left;
    float padR = style.Layout.Padding.Right;
    float availableW = std::max(0.0f, W - (padL + padR));

    // Skip if the field has no width yet (layout not converged).
    if (availableW <= 0.0f)
        return;

    // Try progressively tighter representations:
    // 1. Full precision text
    // 2. Compact with 2 decimal places (50.43B)
    // 3. Compact with 1 decimal place (50.4B)
    // 4. Compact with 0 decimal places (50B)
    const float v = this->GetValue();

    if (m_FixedDecimalPlaces >= 0)
    {
        const std::string fixed = FormatFloat(v, false);
        if (GetEditorText() != fixed)
            SetEditorText(fixed);
        return;
    }

    // Step 1: full display text (7 significant digits — the display-mode
    // counterpart of the compact forms, not the focused editing representation)
    const std::string full = FormatFixedAdaptive(v);
    const float fullW = font->MeasureText(full, px, lsPx).metrics.width;
    if (fullW <= availableW)
    {
        if (GetEditorText() != full)
            SetEditorText(full);
        return;
    }

    // Step 2-4: progressively tighter compact
    constexpr int precisions[] = {2, 1, 0};
    for (const int prec : precisions)
    {
        std::string compact = CompactSuffixAt(v, prec);
        if (compact.empty())
            continue;  // value too small for suffix at this step

        const float compactW = font->MeasureText(compact, px, lsPx).metrics.width;
        if (compactW <= availableW)
        {
            if (GetEditorText() != compact)
                SetEditorText(compact);
            return;
        }
    }

    // Fallback: use the best-effort compact (0 precision)
    std::string fallback = CompactSuffixAt(v, 0);
    if (!fallback.empty() && GetEditorText() != fallback)
        SetEditorText(fallback);
}

inline std::string FloatField::TrimTrailingZeros(std::string s)
{
    const std::size_t dot = s.find('.');
    if (dot != std::string::npos)
    {
        std::size_t last = s.find_last_not_of('0');
        if (last == dot)
            --last;  // nothing after the point -> drop it too
        s.erase(last + 1);
    }
    return s;
}

// Insert a space every three digits in the integer part, grouping from the
// right once the value reaches five digits (e.g. "10000" -> "10 000",
// "50000000" -> "50 000 000"). Leaves the fractional part, sign, or exponent untouched.
static std::string InsertThousandsSpaces(const std::string& s)
{
    const std::size_t dot = s.find('.');
    const std::size_t ePos = s.find('e');
    const std::size_t intEnd = std::min(dot, ePos);  // stop before '.' or 'e'
    const std::size_t intLen = (intEnd == std::string::npos) ? s.size() : intEnd;

    // Find where the digits start (skip an optional leading '-' or '+').
    std::size_t start = 0;
    if (intLen > 0 && (s[0] == '-' || s[0] == '+'))
        ++start;

    const int digitCount = static_cast<int>(intLen - start);
    if (digitCount <= 4)
        return s;  // nothing to group

    // Build the integer part from right-to-left, inserting a space every 3 digits.
    std::string grouped;
    grouped.reserve(static_cast<std::size_t>(digitCount) + static_cast<std::size_t>(digitCount) / 3);
    for (int i = digitCount - 1; i >= 0; --i)
    {
        if ((digitCount - 1 - i) % 3 == 0 && i != digitCount - 1)
            grouped += ' ';
        grouped += s[start + i];
    }
    // Reverse to restore left-to-right order.
    std::reverse(grouped.begin(), grouped.end());

    // Prefix with sign (if any), suffix with fractional/exponent tail.
    std::string result;
    result.reserve(s.size() + grouped.size() - static_cast<std::size_t>(digitCount));
    if (start > 0)
        result += s[0];  // sign
    result += grouped;
    if (intEnd != std::string::npos)
        result.append(s, intEnd, s.size() - intEnd);

    return result;
}

// Format a float with fixed notation using just enough precision to capture
// the significant digits of a 32-bit float (7 sig figs) without dumping
// binary round-off noise (e.g. 50456.456 stays "50456.456", not "...457031").
// Large integers get thousands separators (50 000 000) for readability.
static std::string FormatFixedAdaptive(float v)
{
    const int kSig = 7;  // IEEE-754 float has ~7 decimal significant digits
    const double d = static_cast<double>(v);
    const double ad = std::fabs(d);

    if (ad == 0.0)
        return "0";

    // Choose decimal places so we keep ~kSig significant digits but never more
    // than 6 (keeps things tidy) and never fewer than 0.
    const int places = std::clamp(kSig - static_cast<int>(std::floor(std::log10(ad))) - 1, 0, 6);
    std::string s = FormatFixed(d, places);

    // Group the integer part with thousands separators from five digits upward
    // when there is no fractional component.
    const std::size_t dot = s.find('.');
    if (dot == std::string::npos && FloatField::IsBigNumberSpacingEnabled())
        return InsertThousandsSpaces(s);

    return s;
}

// Shortest text that parses back to the exact same float (std::to_chars):
// the focused/editing representation. Focusing a field must never round the
// value through the 7-significant-digit display form — the user edits (and can
// copy) precisely what is stored. Magnitudes a person types as plain digits
// (1000000000, 0.0005) stay in fixed notation; an exponent appears only
// outside that span.
static std::string FormatFloatRoundTrip(float v)
{
    constexpr float kSmallestFixed = 1e-4f;
    constexpr float kLargestFixed = 1e15f;
    const float magnitude = std::fabs(v);
    const std::chars_format format = (magnitude >= kSmallestFixed && magnitude < kLargestFixed)
                                         ? std::chars_format::fixed
                                         : std::chars_format::general;
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v, format);
    return std::string(buf, res.ptr);
}

// Format a small magnitude in scientific notation, trimming the redundant
// ".00" when there are no significant fractional digits (e.g. "3e-6" not
// "3.00e-06", but "3.01e-6" keeps its decimals).
static std::string FormatScientificTrimmed(float v)
{
    std::ostringstream oss;
    oss << std::scientific << std::setprecision(6) << static_cast<double>(v);
    std::string s = oss.str();

    // Find the 'e' exponent marker (std::scientific uses lower-case 'e').
    const std::size_t ePos = s.find('e');
    if (ePos == std::string::npos)
        return s;

    std::string mantissa = s.substr(0, ePos);
    std::string exponent = s.substr(ePos);  // includes 'e'

    // Trim trailing zeros in the mantissa, then drop a now-pointless decimal point.
    mantissa = FloatField::TrimTrailingZeros(mantissa);

    // Normalize the exponent text: "e-06" -> "e-6", "e+03" -> "e3".
    if (exponent.size() >= 2 && (exponent[1] == '+' || exponent[1] == '-'))
    {
        std::string sign(1, exponent[1]);
        int expNum = std::atoi(exponent.c_str() + 2);
        exponent = "e" + sign + std::to_string(expNum);
    }

    return mantissa + exponent;
}

inline std::string FloatField::FormatFloat(float v, bool compact)
{
    if (m_FixedDecimalPlaces >= 0)
    {
        std::ostringstream oss;
        oss.setf(std::ios::fixed, std::ios::floatfield);
        oss.precision(m_FixedDecimalPlaces);
        oss << v;
        return oss.str();
    }

    // Zero is always "0", never scientific/compact, regardless of mode.
    if (v == 0.0f)
        return "0";

    // Focused (editing): shortest exact round-trip, never the lossy 7-digit
    // display rounding.
    if (!compact)
        return FormatFloatRoundTrip(v);

    // Compact suffix notation — only used by the width-aware OnPostLayout path
    // when the full text overflows the field box. Trailing zeros trimmed so
    // "50.00k" becomes "50k".
    const float av = std::abs(v);
    const bool negative = v < 0.0f;
    const float mag = negative ? av : v;

    auto formatCompact = [&](float divisor, char suffix) -> std::string
    {
        return (negative ? "-" : "") + FormatFixed(mag / divisor, 2) + suffix;
    };

    if (mag >= 1e12f) return formatCompact(1e12f, 'T');
    if (mag >= 1e9f)  return formatCompact(1e9f, 'B');
    if (mag >= 1e6f)  return formatCompact(1e6f, 'M');
    if (mag >= 1e3f)  return formatCompact(1e3f, 'K');

    // Very small values: trimmed scientific notation.
    if (mag < 1e-3f)
        return FormatScientificTrimmed(v);

    // Otherwise: full adaptive-precision fixed text.
    return FormatFixedAdaptive(v);
}

// Convert a single suffix character to its multiplier.
// Returns 1.0 if the character is not a recognized suffix.
static double SuffixToMultiplier(char c)
{
    switch (std::tolower(c))
    {
        case 'k': return 1e3;
        case 'm': return 1e6;
        case 'b': return 1e9;
        case 't': return 1e12;
        default:  return 1.0;
    }
}

inline bool FloatField::TryParseFloat(const std::string& text, float& outValue)
{
    double v = 0.0;

    // First try: does the input end with a compact suffix (k/M/B/T)?
    // Examples: "1.5k", "2M", "-3.2b", "100T"
    if (!text.empty())
    {
        const char last = text.back();
        if (std::isalpha(static_cast<unsigned char>(last)))
        {
            const double mult = SuffixToMultiplier(last);
            if (mult != 1.0)
            {
                // Strip the suffix and try to parse the numeric prefix.
                std::string prefix = text.substr(0, text.size() - 1);
                if (!prefix.empty())
                {
                    double num = 0.0;
                    char* end = nullptr;
                    num = std::strtod(prefix.c_str(), &end);
                    if (end == prefix.c_str())
                    {
                        // strtod failed to consume anything — prefix is not a number.
                        (void)end;
                    }
                    else
                    {
                        v = num * mult;
                        outValue = static_cast<float>(v);
                        return true;
                    }
                }
            }
        }
    }

    // Fallback: standard numeric expression evaluation (handles plain numbers,
    // math expressions like "2+2", scientific notation like "1.5e-3", etc.).
    if (!TryEvaluateNumericExpression(text, v))
        return false;
    outValue = static_cast<float>(v);
    return true;
}

} // namespace GameEngine
