#include "Terrain/TerrainRuleConditionVocabulary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace GameEngine::Editor::TerrainRuleVocabulary
{
namespace
{

// Slope in degrees spans the full quarter turn; a vertical face is 90.
constexpr float32 kSlopeDegreesMax = 90.0f;

// Metres domain when the terrain's HeightScale is unreadable (no terrain bound
// to the inspector yet). The component default is 256 m, so a band authored
// against this one keeps its meaning once a terrain appears.
constexpr float32 kFallbackHeightMetres = 256.0f;

// Ceiling on the digits added to keep a small non-zero value from rendering as
// zero. Three covers a millimetre of terrain; past that the number is noise.
constexpr int kMaxExtraDecimals = 3;

std::string FormatFixed(float32 value, int decimals)
{
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
    return std::string(buffer);
}

// True when a rendered number carries no significant digit — "0", "0.00", "-0".
bool ReadsAsZero(const std::string& text)
{
    return text.find_first_not_of("-+0.") == std::string::npos;
}

} // namespace

int ConditionDecimals(Kind kind)
{
    // Degrees and metres carry one decimal; a [0, 1] band needs two before its
    // handles read as having moved at all. NOT magnitude-dependent: a threshold
    // made 4.8 print "5" and 3.2 print "3", and it made a caption say "34" next
    // to a field saying "34.4". Trailing zeros come off afterwards, so a whole
    // value still reads whole.
    switch (kind)
    {
    case Kind::SlopeDegrees:
    case Kind::HeightMetres:
        return 1;
    case Kind::SlopeNormalized:
    case Kind::HeightNormalized:
    case Kind::Noise:
        break;
    }
    return 2;
}

std::string_view ConditionKindLabel(Kind kind)
{
    switch (kind)
    {
    case Kind::SlopeDegrees:
        return "Slope (degrees)";
    case Kind::SlopeNormalized:
        return "Slope (0-1 raw)";
    case Kind::HeightMetres:
        return "Height (metres)";
    case Kind::HeightNormalized:
        return "Height (0-1 of range)";
    case Kind::Noise:
        return "Noise";
    }
    return "Slope (degrees)";
}

std::string_view ConditionUnitSuffix(Kind kind)
{
    switch (kind)
    {
    case Kind::SlopeDegrees:
        return "\xC2\xB0"; // U+00B0 DEGREE SIGN
    case Kind::HeightMetres:
        return " m";
    case Kind::SlopeNormalized:
    case Kind::HeightNormalized:
    case Kind::Noise:
        break;
    }
    return "";
}

std::string_view ConditionKindHelp(Kind kind)
{
    switch (kind)
    {
    case Kind::SlopeDegrees:
        return "Surface angle from horizontal, 0 flat to 90 vertical - the unit to author slope in.";
    case Kind::SlopeNormalized:
        return "The raw 1 - normal.Y, flattened by the terrain's Height Scale - NOT an angle, so "
               "the same number means a different steepness on every terrain. Author slope in "
               "degrees instead.";
    case Kind::HeightMetres:
        return "Metres above the terrain's base, not world Y - so the band holds still when the "
               "terrain entity moves, and a snow line at 400 m stays at 400 m when the "
               "mountain grows under it.";
    case Kind::HeightNormalized:
        return "Fraction of the terrain's live height range, 0 at its lowest point and 1 at its "
               "highest - so the band CHASES the heightfield, raising a snow line as the "
               "mountain under it grows, and reads correctly at any Height Scale.";
    case Kind::Noise:
        return "Value noise over world XZ from 0 to 1, with its own frequency and seed, to break "
               "up the banding the slope and height kinds produce on their own.";
    }
    return "";
}

ConditionDomain ConditionAuthoringDomain(Kind kind, float32 terrainHeightMetres)
{
    switch (kind)
    {
    case Kind::SlopeDegrees:
        return {0.0f, kSlopeDegreesMax};
    case Kind::HeightMetres:
        return {0.0f, terrainHeightMetres > 0.0f ? terrainHeightMetres : kFallbackHeightMetres};
    case Kind::SlopeNormalized:
    case Kind::HeightNormalized:
    case Kind::Noise:
        break;
    }
    return {0.0f, 1.0f};
}

std::string FormatConditionValue(Kind kind, float32 value)
{
    const int decimals = ConditionDecimals(kind);
    std::string text = FormatFixed(value, decimals);

    // Trailing zeros come off: a caption reading "34.0" beside a field reading
    // "34.4" is the same disagreement the whole-number rounding produced, and a
    // caption reading "34.40" is noise. Whole values keep no decimal point.
    if (text.find('.') != std::string::npos)
    {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }

    // A non-zero value must never render as zero. Metres and degrees round to
    // whole numbers, so a 0.4 m feather would otherwise print "0 m" while the
    // sentence around it calls the band feathered — the reader is then told a
    // hard edge and a soft one in the same breath. Add precision until a
    // significant digit appears rather than pinning one precision per unit.
    if (value != 0.0f)
    {
        for (int extra = 1; extra <= kMaxExtraDecimals && ReadsAsZero(text); ++extra)
        {
            text = FormatFixed(value, decimals + extra);
            if (text.find('.') != std::string::npos)
            {
                text.erase(text.find_last_not_of('0') + 1);
                if (!text.empty() && text.back() == '.')
                    text.pop_back();
            }
        }
    }

    return text + std::string(ConditionUnitSuffix(kind));
}

namespace
{

// What the rule does with the weight the band just described — one sentence, one
// slot, three mutually exclusive answers, all in the same "The rule then ..."
// shape so the caption reads as one voice however the row is set up.
//
// Below full strength the row scales whatever its mode would have given, and the
// percentage is the whole story; naming the mode there as well would have to
// promise "fully replaces" for a row that lerps halfway.
//
// At full strength the two modes reach genuinely different ground, which is why
// "Full weight" alone over-promised. CompositeSplatTexel (TerrainSplatComposite.h)
// is the authority for both: BLEND adds this rule's weight to the texel and
// renormalizes, so a full-weight row against ground holding none of its material
// ends up level with it — the ground keeps half, it is not displaced. REPLACE
// lerps every channel toward the pure material, so at full weight nothing of the
// ground survives.
std::string RuleOutcomeSentence(const Components::TerrainSurfaceRule& rule)
{
    if (rule.Strength < 1.0f)
    {
        const int percent =
            static_cast<int>(std::lround(std::clamp(rule.Strength, 0.0f, 1.0f) * 100.0f));
        return " The rule then scales this to " + std::to_string(percent) + "%.";
    }
    if (rule.Replace)
        return " The rule then fully replaces the ground where it applies.";
    return " The rule then blends this up to an equal share with the ground beneath.";
}

} // namespace

std::string ConditionBandSummary(const Components::TerrainRuleCondition& condition,
                                 const Components::TerrainSurfaceRule& owningRule)
{
    const Kind kind = condition.Kind;
    const std::string low = FormatConditionValue(kind, condition.Min);
    const std::string high = FormatConditionValue(kind, condition.Max);
    const bool feathered = condition.Feather > 0.0f;
    const std::string reach = FormatConditionValue(kind, condition.Feather);
    const char* shape = condition.FalloffCurve == Curve::Smoothstep ? "smoothstep" : "linear";
    const std::string outcome = RuleOutcomeSentence(owningRule);

    // No plateau: the two ramps are the whole of the weight, and with no feather
    // there is no weight at all. Said first because it changes what the rest means.
    if (condition.Min > condition.Max)
    {
        if (!feathered)
            return "Empty band - nothing is between " + low + " and " + high + ", and with no "
                   "feather nothing ramps into it either. Drag the low handle below the high "
                   "one, or give the edges a feather." + outcome;
        return "Empty band - only the " + reach + " " + shape + " feather outside " + high
             + " and " + low + " carries any weight." + outcome;
    }

    // Min == Max is a peak, not a band: the plateau has no width, so describing
    // it as a range would read as a hard-edged nothing.
    if (condition.Min == condition.Max)
    {
        if (!feathered)
            return "Peaks at " + low + " with a hard edge, so only an exact match counts. "
                   "Widen the band or add a feather." + outcome;
        return "Peaks at " + low + ", falling to nothing over a " + reach + " " + shape
             + " feather on each side." + outcome;
    }

    if (!feathered)
        return "Full weight from " + low + " to " + high + ", hard edges - nothing outside."
             + outcome;
    return "Full weight from " + low + " to " + high + ", falling to nothing over a " + reach
         + " " + shape + " feather past each edge." + outcome;
}

std::string RuleInertReason(const Components::TerrainSurfaceRule& rule)
{
    if (rule.Strength <= 0.0f)
        return "This rule is ignored: its Strength is 0, so every texel it matches gets no "
               "weight. Raise Strength above 0.";

    const uint32 conditionCount =
        std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);
    for (uint32 i = 0; i < conditionCount; ++i)
    {
        const Components::TerrainRuleCondition& condition = rule.Conditions[i];
        if (condition.Min > condition.Max && condition.Feather <= 0.0f)
        {
            return "This rule is ignored: condition " + std::to_string(i + 1) + " has its low "
                   "handle above its high one and no feather, so no value can satisfy it and "
                   "the whole rule multiplies to zero. Drag the handles back into order, or "
                   "give the condition a feather.";
        }
    }

    return {};
}

std::string ConditionBandRangeText(const Components::TerrainRuleCondition& condition)
{
    return FormatConditionValue(condition.Kind, condition.Min) + " - "
         + FormatConditionValue(condition.Kind, condition.Max);
}

Components::TerrainRuleCondition MakeDefaultCondition(Kind kind, float32 terrainHeightMetres)
{
    const ConditionDomain domain = ConditionAuthoringDomain(kind, terrainHeightMetres);
    Components::TerrainRuleCondition condition{};
    condition.Kind = kind;
    condition.Min = domain.Min;
    condition.Max = domain.Max;
    return condition;
}

std::string_view UnconditionalRuleNote()
{
    return "No conditions, so this rule covers the whole volume at full strength. "
           "Add a condition to restrict where it applies.";
}

std::string_view SurfaceRulesSectionHelp()
{
    return "Rules apply top to bottom. Within a rule, conditions AND together and their feathers "
           "multiply. Rules composite into the same splat as the effects earlier in this volume's "
           "stack, so at full strength a rule takes ground a paint layer put down - outright in "
           "Replace, down to an equal share in Blend.";
}

} // namespace GameEngine::Editor::TerrainRuleVocabulary
