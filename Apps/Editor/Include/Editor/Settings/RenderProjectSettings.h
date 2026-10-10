#pragma once

#include "Engine/Rendering/AntiAliasingProjectSettings.h"

#include <cstdint>
#include <filesystem>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
}

namespace GameEngine::Editor
{

static constexpr uint32_t kDefaultDirectionalShadowResolution = 2048u;
// Cascade projection mode (rendering.directionalShadowProjection, "stable" |
// "close"). Stable fixes the splits and fits a bounding sphere, so
// worldPerTexel cannot move and the shadow texel lattice stays welded to the
// world; Close follows the SDSM depth bounds with a tight AABB for a sharper
// map that re-quantizes as the camera moves. Defaults to Stable: a crawling
// shadow edge is a correctness-grade artifact, Close's sharpness is an
// optimisation -- but Stable's cost is content-dependent and can be severe
// (2.5x coarser cascade-0 texels at a ground-level pose on measured content),
// so it is opt-in rather than the default.
static constexpr bool kDefaultDirectionalShadowStableProjection = false;

class SettingsStore;

// Reads/writes the directional-shadow member of the already-loaded project
// SettingsStore. The caller owns Load()/Save(), allowing the settings page and
// the renderer apply path to share one store transaction with sibling render
// settings instead of performing a shadow-only filesystem load.
uint32_t GetDirectionalShadowResolution(const SettingsStore& store);
void SetDirectionalShadowResolution(SettingsStore& store, uint32_t resolution);

// Same contract, for the cascade projection mode. True selects Stable.
bool GetDirectionalShadowStableProjection(const SettingsStore& store);
void SetDirectionalShadowStableProjection(SettingsStore& store, bool stable);

// One-shot: gives a project that has never chosen an anti-aliasing mode the
// best one this device can run and WRITES it — rendering.aaMode, plus
// rendering.msaa on the MSAA rungs — so the choice is made once and is an
// ordinary stored setting the user edits from then on. Saves the store itself
// and reports whether it wrote.
//
// It also rewrites the retired "auto" tokens — the values rendering.aaMode and
// rendering.msaa could carry that named a per-frame resolution rather than a
// choice — and the two are retired on different terms, because they need
// different things to answer them. A retired MODE is answered by the device, so
// caps that say nobody answered leave it alone: nothing is decided before
// something can answer, and a device-less host leaves the key absent for a later
// open rather than burning TAA in. A retired or otherwise unusable COUNT is
// answered by the vocabulary alone, so it is rewritten on any host AND under a
// mode the project has already chosen — the one case where an early return on
// "this project chose" would keep the token in the file forever.
//
// Otherwise a project that names a concrete mode is never touched.
//
// The values that land are CONCRETE and travel with the project. A project
// created on an 8x-capable machine carries msaa=4 to a machine that caps at 2x,
// where SetDefaultMSAASampleCount halves it to that device's maximum; a machine
// with no MSAA at all halves it to 1 and the scene view renders unantialiased
// rather than falling back to TAA. By then the mode is the project's stored
// choice, and re-deriving it per machine is the per-frame resolution this
// replaced.
bool MaterializeDefaultAntiAliasing(SettingsStore& store,
                                    Engine::Renderer::AntiAliasingDeviceCaps caps);

// The project's AA + render-scale settings (rendering.aaMode / msaa /
// fxaaQuality / taaRenderScale / drsMode / drsTargetFps), read and written as
// the one struct that owns their serialized shape. The settings page edits a
// whole snapshot rather than a key at a time, so the file cannot end up in a
// state the struct cannot describe.
//
// The struct also carries ModeChosen, which is how "this project has never
// chosen a mode" reaches both MaterializeDefaultAntiAliasing above and the
// in-memory fallback in ApplyProjectRenderSettings — one reader, so the two
// cannot disagree about which projects are undecided.
//
// The path overloads open and save the project store themselves; Save returns
// false for an empty workspace root (nothing to write to) and for an I/O
// failure, which it logs.
Rendering::AntiAliasingProjectSettings GetAntiAliasingSettings(const SettingsStore& store);
void SetAntiAliasingSettings(SettingsStore& store,
                             const Rendering::AntiAliasingProjectSettings& settings);
Rendering::AntiAliasingProjectSettings LoadProjectAntiAliasingSettings(
    const std::filesystem::path& workspaceRoot);
bool SaveProjectAntiAliasingSettings(const std::filesystem::path& workspaceRoot,
                                     const Rendering::AntiAliasingProjectSettings& settings);

// Applies an already-loaded project settings snapshot. The all-window editor
// path uses this overload so every window receives the exact same snapshot and
// the project file is read only once for the fan-out. Read-only: the fan-out's
// owner calls MaterializeDefaultAntiAliasing on the store first, so the write
// happens once rather than once per window.
void ApplyProjectRenderSettings(Engine::Renderer::RenderServices& renderServices,
                                const SettingsStore& store);

// Applies every renderer knob the project's settings file owns to one
// RenderServices: MSAA sample count (rendering.msaa), AA mode
// (rendering.aaMode), TAAU render scale (rendering.taaRenderScale),
// render-scale mode + dynamic target (rendering.drsMode / drsTargetFps),
// directional shadow resolution (rendering.directionalShadowResolution), and
// mesh LOD selection + budgets (rendering.lodMode / lodErrorBudgetPx /
// lodSkinnedBudgetScale).
//
// Applies ALL of them: the renderer's state for these knobs afterwards is a
// total function of projectRoot's settings file, because an absent key applies
// the engine default instead of skipping the setter. That totality is what
// makes the call correct to repeat when the open project changes, where
// RenderServices still holds the OUTGOING project's values. The settings page
// reads its displayed values back from the project file, so a partial apply
// shows a value the renderer is not using — and the page cannot self-heal,
// because re-selecting an already-selected dropdown entry fires no callback.
//
// Call once per window: a floating window owns a private RenderServices, and
// MeshGPURegistry (which carries the LOD mode) is a member of it.
//
// rendering.hdr is deliberately absent: HDR output is per-window swapchain
// state owned by HdrOutputController, not a RenderServices knob, and its
// refresh re-reads the file itself. Paths that need the full project state
// (SetProjectFolder) pair this apply with RequestHdrOutputRefreshForAllWindows.
//
// GE_AA_MODE and GE_TAA_RENDER_SCALE outrank the project settings; both exist
// for A/B harness runs and are read in RenderServices::Initialize. Their reach
// differs, and the difference is documented and enforced where it is applied,
// on AntiAliasingProjectSettings::Apply{AntiAliasing,RenderScale}To — not here,
// because the Player needs the same precedence and never runs this function.
void ApplyProjectRenderSettings(Engine::Renderer::RenderServices& renderServices,
                                const std::filesystem::path& projectRoot);

} // namespace GameEngine::Editor
