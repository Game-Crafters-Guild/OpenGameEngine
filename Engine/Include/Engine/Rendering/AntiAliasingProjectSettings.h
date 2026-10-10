#pragma once

#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/DynamicResolutionController.h"

#include <filesystem>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Rendering
{

// The project's engine-wide anti-aliasing and render-scale settings, and the
// sole owner of their serialized shape. Two transports carry the same object
// (the LodProjectSettings pattern):
//
//   authored  <ProjectRoot>/.Editor/ProjectSettings.json -> "rendering": { ... }
//   shipped   <exe dir>/game.config                      -> "rendering": { ... }
//
//   { "rendering": { "aaMode": "taa", "msaa": "4", "fxaaQuality": "quality",
//                    "taaRenderScale": 1.5, "drsMode": "fixed",
//                    "drsTargetFps": 60 } }
//
// The editor authors the first; BuildPipeline copies it into the second at
// package time, because `.Editor/` never stages into a built game. The Player
// applies it to RenderServices at boot, so a shipped game runs the same AA
// stack the editor previewed. Per-camera Camera::AntiAliasing / MSAASamples
// overrides still win per view.
//
// SSAA has no token of its own: it IS mode Off with RenderScale above 1.0, and
// the settings page derives its dropdown entry from that pair.

// The persisted rendering.aaMode vocabulary — "off" | "msaa" | "taa" | "fxaa" |
// "smaa" | "temporalfxaa" — shared by ReadFrom/WriteTo, the settings page, and
// the debug-server IPC. Unrecognized input parses as Off (which is what the editor-level "ssaa"
// of older project files means, its render scale carrying the supersampling).
// Neither key has an "auto": a retired aaMode "auto" and a retired msaa "auto"
// both read as "the file named nothing" (ModeChosen / SamplesChosen below), and
// the editor's MaterializeDefaultAntiAliasing rewrites both concretely on
// project open — the mode against the device, the count against the vocabulary.
// WriteTo never emits either.
const char* ToAAModeToken(Engine::Renderer::AntiAliasingMode mode);
Engine::Renderer::AntiAliasingMode ParseAAModeToken(std::string_view token);

struct AntiAliasingProjectSettings
{
    // False when the project has never chosen an anti-aliasing mode, which is
    // not the same as choosing Off. Such a project takes the capability ladder
    // instead of AAMode below: the editor resolves it once and WRITES it
    // (MaterializeDefaultAntiAliasing), and every apply resolves it in memory
    // meanwhile (ApplyAntiAliasingTo).
    bool ModeChosen = false;
    Engine::Renderer::AntiAliasingMode AAMode = Engine::Renderer::AntiAliasingMode::Off;
    // Always a concrete count. A file naming none — including one carrying the
    // retired "auto" — parses as kDefaultMsaaSampleCount, which
    // SetDefaultMSAASampleCount then halves down to this device's cap. There is
    // no sentinel: the resolution belongs to the reader, not to the value.
    uint32 MsaaSamples = Engine::Renderer::kDefaultMsaaSampleCount;
    // True only when the file named a count this vocabulary understands. False
    // for an absent key, for the retired "auto", and for anything unreadable —
    // MsaaSamples then carries the default above rather than a stored value.
    // The editor's MaterializeDefaultAntiAliasing is the one reader: it rewrites
    // an unusable stored count concretely instead of re-deriving the vocabulary
    // here.
    bool SamplesChosen = false;
    Engine::Renderer::FxaaQuality FxaaQualityValue = Engine::Renderer::FxaaQuality::Quality;
    // [kMinRenderScale, kMaxRenderScale]; above 1.0 = supersampling (SSAA).
    float RenderScale = 1.0f;
    Engine::Renderer::DynamicResolutionMode ScaleMode =
        Engine::Renderer::DynamicResolutionMode::Off;
    float DrsTargetFps = 60.0f;

    static AntiAliasingProjectSettings ReadFrom(const nlohmann::json& rendering);
    // ReadFrom's inverse for the MODE, ModeChosen included: an unchosen snapshot
    // writes NEITHER aaMode nor msaa, and clears them if the object it writes
    // over carries them, because absence is how "never chose" is spelled on
    // disk. Anything else would decide a project's anti-aliasing as a side
    // effect of saving an unrelated setting.
    //
    // The COUNT round-trips only under a chosen mode. An unchosen snapshot's
    // count is written away with the mode, so {"msaa": "off"} reads back as the
    // default rather than as 1 — deliberately: a count under no mode is inert
    // (the ladder supplies both), and keeping it would leave the legacy "a count
    // above 1 means MSAA" rule reading a value the writer invented.
    void WriteTo(nlohmann::json& rendering) const;
    static AntiAliasingProjectSettings Load(const std::filesystem::path& workspaceRoot);

    // The AA knobs alone: mode, MSAA sample count, FXAA quality. All plain
    // assignments — the renderer re-resolves them next frame.
    //
    // This is the ONE place an unchosen mode (ModeChosen false) turns into a
    // real one: it asks the renderer's device for the capability ladder's rung
    // (ResolveDefaultAntiAliasing), so every host gets that default without
    // restating it — including the Player, whose game.config the editor never
    // materialized.
    //
    // It is also where GE_AA_MODE outranks the file. Precedence is env >
    // authored > ladder, and it holds in EVERY host because the guard is here:
    // the Player applies its snapshot straight after RenderServices::Initialize
    // with nothing in between, so a guard living in a host would leave the
    // override dead there. GE_MSAA_SAMPLES is NOT in this table — it seeds the
    // startup count and mode at Initialize and an applied snapshot overwrites
    // it, in both hosts alike.
    void ApplyAntiAliasingTo(Engine::Renderer::RenderServices& renderServices) const;
    // The render-scale knobs: scale, scale mode, dynamic target. Applied
    // scale-mode Off first to normalize, then the scale, then the mode — so
    // entering Fixed records the scale as the value leaving Dynamic restores.
    //
    // Separate from the AA half because the two are separately suppressible:
    // GE_TAA_RENDER_SCALE pins the scale, and every setter in here writes it
    // (Off pins 1.0, leaving Dynamic restores its remembered value), so under
    // that override the whole half is skipped, not merely re-fed its own
    // current values. Skipped here, for the same reason the AA half's guard is
    // here: every host has to honour it.
    void ApplyRenderScaleTo(Engine::Renderer::RenderServices& renderServices) const;
    // Both halves — what a boot-time apply wants.
    void ApplyTo(Engine::Renderer::RenderServices& renderServices) const;
};

} // namespace GameEngine::Rendering
