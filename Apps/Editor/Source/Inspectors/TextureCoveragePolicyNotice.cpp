#include "Inspectors/TextureCoveragePolicyNotice.h"

#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/UIElement.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

// The consequence every unsatisfiable case shares. The cook refuses an invalid policy rather
// than shipping uncorrected mips, and the refusal reaches the whole asset, so this is the
// sentence an author needs before they leave the panel.
constexpr const char* kUnsatisfiedConsequence = " Until that is changed this texture will not load.";

// Which compression choices can carry alpha through to the mip chain. Auto is admitted here
// and refused per-usage below, because Auto resolves a mask to a single-channel format.
bool CompressionKeepsAlpha(TextureCookCompression compression)
{
    return compression == TextureCookCompression::Auto ||
           compression == TextureCookCompression::None ||
           compression == TextureCookCompression::BC7;
}

} // namespace

bool TextureCoverageRefusesCompression(TextureCookCompression compression)
{
    return !CompressionKeepsAlpha(compression);
}

bool TextureCoverageRefusesUsage(TextureCookUsage usage)
{
    return usage == TextureCookUsage::Normal;
}

TextureCoveragePolicyState ReadTextureCoveragePolicy(const TextureCoveragePolicyInputs& inputs)
{
    TextureCoveragePolicyState state;

    TextureCookSettings settings;
    settings.Compression = inputs.Compression;
    settings.Usage = inputs.Usage;

    std::string parseError;
    if (!ParseTextureAlphaCoverageMeta(inputs.Enabled, inputs.Cutoff, settings, parseError))
    {
        // Stored text the panel cannot read is exactly what the cook will refuse, so name the
        // value rather than showing the rows as if the policy were simply off.
        state.Enabled = true;
        state.Satisfied = false;
        state.Notice = "Preserve Alpha Coverage is stored as a value this texture cannot use: " +
                       parseError + "." + kUnsatisfiedConsequence;
        return state;
    }

    state.Enabled = settings.PreserveAlphaCoverage;
    state.Cutoff = settings.AlphaCoverageCutoff;

    // The cutoff row shows the stored number even while the policy is off, so an author can set
    // it before switching the policy on.
    if (!state.Enabled)
    {
        TextureCookSettings displayed;
        std::string ignoredError;
        if (ParseTextureAlphaCoverageMeta("1", inputs.Cutoff, displayed, ignoredError))
            state.Cutoff = displayed.AlphaCoverageCutoff;
        return state;
    }

    if (inputs.SourceIsHighDynamicRange)
    {
        state.Satisfied = false;
        state.Notice = std::string(
            "Preserve Alpha Coverage corrects low dynamic range alpha, and this source is high "
            "dynamic range.") + kUnsatisfiedConsequence;
        return state;
    }

    if (TextureCoverageRefusesUsage(inputs.Usage))
    {
        state.Satisfied = false;
        state.Notice = std::string(
            "Preserve Alpha Coverage cannot correct a normal map. Set Usage to Color, Packed or "
            "Auto.") + kUnsatisfiedConsequence;
        return state;
    }

    if (TextureCoverageRefusesCompression(inputs.Compression))
    {
        state.Satisfied = false;
        state.Notice = std::string(
            "Preserve Alpha Coverage needs a format that keeps alpha, and this compression "
            "discards it. Set Compression to Auto, BC7 or Uncompressed.") + kUnsatisfiedConsequence;
        return state;
    }

    if (inputs.Compression == TextureCookCompression::Auto &&
        inputs.Usage == TextureCookUsage::Mask)
    {
        state.Satisfied = false;
        state.Notice = std::string(
            "Preserve Alpha Coverage cannot use Auto compression with Mask usage, because Auto "
            "resolves a mask to a single-channel format. Set Compression to BC7 or Uncompressed, "
            "or change Usage.") + kUnsatisfiedConsequence;
        return state;
    }

    return state;
}

TextureCoveragePolicyRows AddTextureCoveragePolicyRows(
    UIElement* parent,
    bool settingsWritable,
    std::function<TextureCoveragePolicyInputs()> readInputs,
    std::function<void(bool)> onEnabledChanged,
    std::function<void(float)> onCutoffChanged)
{
    TextureCoveragePolicyRows rows;
    if (!parent || !readInputs)
        return rows;

    // The rows' own callbacks refresh the block after they write, and the refresh needs the
    // rows, so the hook is shared and filled in once they exist.
    auto refresh = std::make_shared<std::function<void()>>();
    rows.OnPolicyChanged = std::make_shared<std::function<void()>>();

    // Built directly rather than through AddTextBlock because this notice is updated in place
    // rather than rebuilt with the panel, so the block needs the element back.
    auto noticeOwned = std::make_unique<Label>();
    noticeOwned->AddClass("inspector-warning");
    rows.Notice = noticeOwned.get();
    parent->AddChild(std::move(noticeOwned));

    rows.Enable = InspectorDrag::AddToggleRow(parent, "Preserve Alpha Coverage", false,
        [onEnabledChanged, refresh, onPolicyChanged = rows.OnPolicyChanged](bool on) {
            if (onEnabledChanged)
                onEnabledChanged(on);
            if (*refresh)
                (*refresh)();
            if (*onPolicyChanged)
                (*onPolicyChanged)();
        },
        "Scale mip alpha so distant levels keep the source's alpha-tested texel coverage. "
        "Ordinary mips drift from it in either direction: thinner where the material's cutoff "
        "sits above the local alpha density, fatter where it sits below and the gaps inside a "
        "cluster fill in as the filter coarsens. Opt-in, for low dynamic range source images "
        "cooked to BC7 or uncompressed. Coarse mips and filtering can still change coverage.");

    // The policy owns whether the cutoff means anything: the cook ignores a stored cutoff while
    // the policy is off, so a write there would spend a recook on nothing.
    rows.Cutoff = InspectorDrag::AddFloatRowWithDrag(parent, "Coverage Cutoff", 0.5f,
        [](float) {},
        [readInputs, onCutoffChanged, refresh](float value) {
            if (!onCutoffChanged || !ReadTextureCoveragePolicy(readInputs()).Enabled)
                return;
            onCutoffChanged(value);
            if (*refresh)
                (*refresh)();
        },
        0.5f, "Reference alpha cutoff for this texture. Alpha equal to the cutoff passes. "
        "Match the material's authored cutoff; the texture never infers it from bindings.",
        0.0f, 1.0f);

    Label* noticeLabel = rows.Notice;
    Toggle* enableToggle = rows.Enable;
    FloatField* cutoffField = rows.Cutoff;
    enableToggle->SetDisabled(!settingsWritable);
    *refresh = [noticeLabel, enableToggle, cutoffField, readInputs, settingsWritable]() {
        const TextureCoveragePolicyState state = ReadTextureCoveragePolicy(readInputs());
        noticeLabel->SetText(state.Notice);
        if (state.Notice.empty())
            noticeLabel->AddClass("hidden");
        else
            noticeLabel->RemoveClass("hidden");
        enableToggle->SetValueWithoutNotify(state.Enabled);
        cutoffField->SetValueWithoutNotify(state.Cutoff);
        cutoffField->SetEnabled(settingsWritable && state.Enabled);
    };
    rows.Refresh = [refresh]() { (*refresh)(); };
    rows.Refresh();
    return rows;
}

} // namespace GameEngine::Editor
