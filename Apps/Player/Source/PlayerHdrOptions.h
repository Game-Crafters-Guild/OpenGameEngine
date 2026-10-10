#pragma once

#include "Rendering/Core/Device.h"

#include <optional>
#include <string>

namespace GameEngine
{

/// The HDR output settings a shipped game.config carries — the lowest-priority
/// layer of the Player's HDR resolution.
struct PlayerHdrProjectSettings
{
    bool Enabled = false;
    Rendering::HdrOutputMode Mode = Rendering::HdrOutputMode::HDR10_PQ;
    Rendering::HdrSwapchainBitDepth BitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
};

/// The HDR-related environment variables, captured as values so the resolution
/// below is a pure function of its arguments. Reading the environment stays at
/// the call site, mirroring Editor::ResolveHdrOutputRequest, which likewise
/// takes the operator override already parsed.
///
/// An empty value is "not set": both in-tree precedents treat it that way
/// (HdrOutputController.cpp's GE_FORCE_HDR_OUTPUT, ApplyWindowModeOverrides'
/// GE_WINDOW_MODE), and a variable exported empty by a shell wrapper must not
/// outrank the launch command.
struct PlayerHdrEnvironment
{
    std::optional<std::string> Mode;     // GE_HDR_MODE
    std::optional<std::string> Enabled;  // GE_HDR
    std::optional<std::string> BitDepth; // GE_HDR_BIT_DEPTH
    std::optional<std::string> Toggle;   // GE_PLAYER_HDR_TOGGLE
};

/// The resolved request, with the origin of each value named for the startup log.
///
/// The sources are string literals with static storage — the flag or variable
/// the value actually came from ("--hdr-mode", "GE_HDR_MODE", "game.config"),
/// not a category — because an operator debugging a run needs to know which
/// input to edit. Same shape as the editor's debug-port `portSource`.
struct PlayerHdrOptions
{
    bool Enabled = false;
    Rendering::HdrOutputMode Mode = Rendering::HdrOutputMode::Off;
    Rendering::HdrSwapchainBitDepth BitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
    bool AllowRuntimeToggle = false;

    const char* ModeSource = "game.config";
    const char* BitDepthSource = "game.config";
};

/// Resolves the Player's HDR output request from the launch command, the
/// environment, and the shipped game.config.
///
/// Precedence is command line > environment > game.config. The flag is the most
/// explicit thing an operator can state and names one run; an environment
/// variable is ambient and is routinely inherited by a whole shell, so a launch
/// that asks for a mode must not be overruled by a variable it never mentioned.
/// (`ApplyWindowModeOverrides` in the same startup path already resolves
/// --window-mode against GE_WINDOW_MODE this way.)
///
/// An unrecognized value is REFUSED, never parsed as Off: silently disabling HDR
/// on a typo is the failure these controls exist to avoid, and on the Player it
/// lands on the shipped surface where no editor UI can contradict it. A refused
/// layer does not speak, and the next layer down governs — which on the editor's
/// two-layer override reduces to the recorded call, warn and fall back to the
/// project setting.
///
/// An enabled request never carries mode Off; it normalizes to Auto, so callers
/// can read `Mode` alone without re-checking `Enabled` — the same normalization
/// Editor::ResolveHdrOutputRequest guarantees.
///
/// Diagnostics for refused values are logged here, so every caller reports a
/// typo identically.
PlayerHdrOptions ResolvePlayerHdrOptions(int argc,
                                         const char* const* argv,
                                         const PlayerHdrEnvironment& environment,
                                         const PlayerHdrProjectSettings& project);

/// Normalizes one raw environment read: absent OR empty is "not set".
///
/// The empty case is real even where a platform cannot produce it through
/// getenv — on Windows/MSVC both `_putenv_s(name, "")` and
/// `SetEnvironmentVariableA(name, "")` DELETE the variable, so getenv never
/// yields an empty string there, while POSIX `setenv(name, "", 1)` does. The
/// rule lives here so it holds on every platform and is testable on all of them.
std::optional<std::string> PlayerHdrEnvironmentValue(const char* raw);

/// Reads the HDR environment variables this process was launched with.
PlayerHdrEnvironment ReadPlayerHdrEnvironment();

/// Emits the single startup line stating the resolved mode and where it came from.
void LogPlayerHdrOptions(const PlayerHdrOptions& options);

} // namespace GameEngine
