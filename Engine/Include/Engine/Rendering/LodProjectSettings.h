#pragma once

#include "Engine/Rendering/MeshLODThresholds.h"

#include <filesystem>
#include <optional>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Rendering
{

// The project's mesh-LOD selection settings, and the sole owner of their
// serialized shape. Two transports carry the same object:
//
//   authored  <ProjectRoot>/.Editor/ProjectSettings.json -> "rendering": { ... }
//   shipped   <exe dir>/game.config                      -> "rendering": { ... }
//
//   { "rendering": { "lodMode": "sse", "lodErrorBudgetPx": 10.0,
//                    "lodSkinnedBudgetScale": 0.75 } }
//
// The editor authors the first; BuildPipeline copies it into the second at
// package time, because `.Editor/` is editor metadata and never stages into a
// built game. Key names, ranges, defaults and the apply live here once, so the
// two transports cannot drift and a new knob reaches a built game by being
// added to this struct rather than to a ship path.
//
// Note the nesting: the editor's SettingsStore key helpers are flat and treat a
// dot as a literal character, so these go through the json object directly.
//
// An absent key means "leave the engine default standing" rather than "zero" —
// that is what the optionals encode, and it keeps a project that never opened
// the page byte-identical to one with no rendering settings at all.
struct LodProjectSettings
{
    LodSelectionMode SelectionMode = LodSelectionMode::Sse;
    std::optional<float> ErrorBudgetPx;
    std::optional<float> SkinnedBudgetScale;
    // Per-view-class budget overrides. Class-level by design: "Scene View"
    // means every scene viewport (main, quad panes, floating windows), which
    // is what Rendering::ViewPurpose distinguishes. Per-individual-viewport
    // granularity would need a new discriminator on ViewDesc and is not
    // offered. Asset previews and thumbnails are deliberately absent — see
    // LodSettingsPage for why a budget knob there is inert.
    Rendering::LodViewBudgetOverride GameViewBudget;
    Rendering::LodViewBudgetOverride SceneViewBudget;
    // Dithered LOD crossfade duration in seconds (see LodSettingsPage's row
    // tooltip). Absent falls back to kDefaultCrossfadeDuration; 0 is off.
    std::optional<float> CrossfadeDuration;
    // LOD dwell band as a fraction of the switch coverage (0 = off, stateless
    // selection). Gaining detail requires coverage to clear a threshold by
    // (1+band); holding or losing it requires only the threshold — stops a
    // camera hovering at a threshold from thrashing between two levels.
    std::optional<float> HysteresisBand;

    static constexpr const char* kRenderingKey = "rendering";
    static constexpr const char* kModeKey = "lodMode";
    static constexpr const char* kErrorBudgetPxKey = "lodErrorBudgetPx";
    static constexpr const char* kSkinnedBudgetScaleKey = "lodSkinnedBudgetScale";
    static constexpr const char* kGameViewBudgetEnabledKey = "lodGameViewBudgetEnabled";
    static constexpr const char* kGameViewBudgetPercentKey = "lodGameViewBudgetPercent";
    static constexpr const char* kSceneViewBudgetEnabledKey = "lodSceneViewBudgetEnabled";
    static constexpr const char* kSceneViewBudgetPercentKey = "lodSceneViewBudgetPercent";
    static constexpr const char* kCrossfadeDurationKey = "lodCrossfadeDuration";
    static constexpr const char* kHysteresisBandKey = "lodHysteresisBand";

    // Persisted ranges. The settings sliders use exactly these bounds: a
    // narrower slider would clamp a hand-edited value on page open and write
    // the clamped one back.
    static constexpr float kMinErrorBudgetPx = 0.0f;
    static constexpr float kMaxErrorBudgetPx = 100.0f;
    static constexpr float kMinSkinnedBudgetScale = 0.05f;
    static constexpr float kMaxSkinnedBudgetScale = 1.0f;
    // The percent slider bounds ARE the engine's clamp range, so a value the
    // engine would clamp cannot be stored or shown.
    static constexpr float kMinBudgetPercent = Rendering::kMinLodBudgetPercent;
    static constexpr float kMaxBudgetPercent = Rendering::kMaxLodBudgetPercent;
    // Crossfade duration. 0 is off; the upper bound is a usability limit, not a
    // technical one — a fade longer than this reads as a permanently stippled
    // object rather than a transition.
    static constexpr float kMinCrossfadeDuration = 0.0f;
    static constexpr float kMaxCrossfadeDuration = 2.0f;
    // The engine default this struct falls back to when the key is absent.
    // RenderServices initializes its duration from this same constant, so
    // applying an absent key to a fresh renderer cannot change it.
    static constexpr float kDefaultCrossfadeDuration = 0.25f;
    // Dwell band. 0 is off and is the default; 1 (gaining a level needs DOUBLE
    // the switch coverage) is a usability bound — past it the band stops
    // reading as stability and starts reading as a stuck coarse level. The
    // measured sweet spot is 0.25 (lod-transition design, step 2).
    static constexpr float kMinHysteresisBand = 0.0f;
    static constexpr float kMaxHysteresisBand = 1.0f;
    static constexpr float kDefaultHysteresisBand = 0.0f;

    // Reads this struct's keys out of a "rendering" object. Out-of-range values
    // clamp and an unrecognized mode token keeps the default, so neither
    // transport can hand the renderer a value it would have to defend against.
    static LodProjectSettings ReadFrom(const nlohmann::json& rendering);

    // Writes just this struct's keys into a "rendering" object, so a sibling
    // rendering.* setting written by anything else survives.
    void WriteTo(nlohmann::json& rendering) const;

    // Read-only load from <workspaceRoot>/.Editor/ProjectSettings.json — the
    // editor's startup apply and the build pipeline's cook. The WRITE side is
    // Editor::SaveLodProjectSettings: atomic writes and the schemaVersion
    // convention are SettingsStore's, and the settings page is the only writer.
    static LodProjectSettings Load(const std::filesystem::path& workspaceRoot);

    // The value in effect for a field whose key is absent — the engine's
    // constructed default. Lets a UI row show what the renderer is actually
    // doing without materializing the key.
    float EffectiveErrorBudgetPx() const;
    float EffectiveSkinnedBudgetScale() const;
    float EffectiveCrossfadeDuration() const;
    float EffectiveHysteresisBand() const;

    // Applies every LOD knob this struct owns, and applies ALL of them: the
    // renderer's LOD state afterwards is a total function of this struct. That
    // is what makes re-applying on a project switch correct — an absent key
    // applies the engine default (the Effective* accessors above), so the
    // outgoing project's budget cannot stay standing behind the incoming
    // project's mode, and the page's displayed value stays true.
    //
    // Safe before any mesh is registered (the mode sticks and later
    // registrations derive under it) and safe on a live renderer, where the
    // mode switch re-derives the registered rows in place.
    void ApplyTo(Engine::Renderer::RenderServices& renderServices) const;
};

} // namespace GameEngine::Rendering
