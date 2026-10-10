#pragma once

#include "Assets/TextureCook.h"

#include <functional>
#include <memory>
#include <string>

namespace GameEngine
{
class FloatField;
class Label;
class Toggle;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor
{

// Preserve Alpha Coverage is one toggle in the Texture Inspector, but whether it can be
// honoured depends on two rows above it: a compression that discards alpha, or a normal-map
// usage, makes the policy unsatisfiable, and an unsatisfiable policy refuses the texture's
// next load rather than quietly cooking uncorrected mips. Those rows and the toggle are
// separate controls, so nothing about the panel tells the author that the pair they just
// built has taken the asset offline.
//
// This is the derived consequence, rendered AT the rows that cause it and re-derived after
// every write from the panel. Kept in its own translation unit, like the other inspector
// notices, so the wording and the re-derivation are reachable from a test rather than only
// from a running editor.

// What the alpha-coverage rows render from. The panel reads these from asset metadata; a
// test supplies them directly.
struct TextureCoveragePolicyInputs
{
    std::string Enabled;  // assets.texture.alphaCoverage, exactly as stored
    std::string Cutoff;   // assets.texture.alphaCutoff, exactly as stored
    TextureCookCompression Compression = TextureCookCompression::Auto;
    TextureCookUsage Usage = TextureCookUsage::Auto;
    bool SourceIsHighDynamicRange = false;
};

// Whether the policy is on, whether the rest of the import settings can carry it, the cutoff
// to show, and the sentence to show when they cannot.
struct TextureCoveragePolicyState
{
    bool Enabled = false;
    bool Satisfied = true;
    float Cutoff = 0.5f;
    std::string Notice;  // empty unless the policy is on and cannot be honoured
};

// Reads the policy out of the stored metadata and names what is wrong when it cannot be
// honoured: what to change, then what happens until it is changed.
TextureCoveragePolicyState ReadTextureCoveragePolicy(const TextureCoveragePolicyInputs& inputs);

// True when this compression choice can never carry an enabled policy, so the inspector marks
// the entry instead of letting the author pick it blind. Auto counts: it resolves to a
// single-channel format for mask usage.
bool TextureCoverageRefusesCompression(TextureCookCompression compression);

// True when this usage choice can never carry an enabled policy.
bool TextureCoverageRefusesUsage(TextureCookUsage usage);

// The alpha-coverage rows and the hook that re-reads the inputs and updates them in place.
struct TextureCoveragePolicyRows
{
    Label* Notice = nullptr;
    Toggle* Enable = nullptr;
    FloatField* Cutoff = nullptr;
    // Re-reads the inputs and updates the notice text and visibility, the toggle, and whether
    // the cutoff row is live. Every row that can invalidate the policy calls this after its
    // write, so the panel never shows a stale verdict.
    std::function<void()> Refresh;
    // Assigned by the owner after the block is built, and called after the enable toggle has
    // written and the block has refreshed: panel state outside the block that turns on the
    // policy being on or off, such as marking the entries the policy cannot use.
    std::shared_ptr<std::function<void()>> OnPolicyChanged;
};

// Adds the notice, the enable toggle and the reference cutoff under `parent`.
// `settingsWritable` is whether the asset's source accepts metadata writes at
// all: it combines with the policy's own gate on the cutoff row, so a refresh
// cannot hand a live cutoff field back on an asset nothing here can save.
TextureCoveragePolicyRows AddTextureCoveragePolicyRows(
    UIElement* parent,
    bool settingsWritable,
    std::function<TextureCoveragePolicyInputs()> readInputs,
    std::function<void(bool)> onEnabledChanged,
    std::function<void(float)> onCutoffChanged);

} // namespace GameEngine::Editor
