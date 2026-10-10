#include "PlayerHdrOptions.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace GameEngine
{
namespace
{

// Named in every refusal message so an operator reading the log learns the fix
// without going to the source.
constexpr const char* kRecognizedModes = "Off, Auto, HDR10_PQ, HLG, scRGB, HDR10+";
constexpr const char* kRecognizedBitDepths = "10 or 16";
constexpr const char* kRecognizedBooleans = "0/1, false/true, no/yes, off/on";

// Lowercase and drop separators and ALL whitespace. Values reaching here come
// from shells, CI scripts and CRLF files, so a trailing carriage return must not
// be the reason an operator's explicit request is refused.
std::string Normalize(std::string_view value)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                    [](unsigned char c) {
                                        return c == '-' || c == '_' || std::isspace(c) != 0;
                                    }),
                     normalized.end());
    return normalized;
}

std::optional<Rendering::HdrSwapchainBitDepth> TryParseBitDepth(std::string_view text)
{
    const std::string value = Normalize(text);
    if (value == "10" || value == "bit10" || value == "rgb10a2")
        return Rendering::HdrSwapchainBitDepth::Bit10;
    if (value == "16" || value == "float16" || value == "fp16" || value == "rgba16f")
        return Rendering::HdrSwapchainBitDepth::Float16;
    return std::nullopt;
}

// Refuses anything it does not recognize rather than folding it to one side: a
// misspelled "flase" that reads as true is the same silent-misinterpretation
// failure as a misspelled mode reading as Off.
std::optional<bool> TryParseBoolean(std::string_view text)
{
    const std::string value = Normalize(text);
    if (value == "0" || value == "false" || value == "no" || value == "off" || value == "disabled")
        return false;
    if (value == "1" || value == "true" || value == "yes" || value == "on" || value == "enabled")
        return true;
    return std::nullopt;
}

struct HdrRequest
{
    bool Enabled = false;
    Rendering::HdrOutputMode Mode = Rendering::HdrOutputMode::Off;
};

// A modal input names the mode outright.
void ApplyMode(HdrRequest& request, Rendering::HdrOutputMode mode)
{
    request.Mode = mode;
    request.Enabled = mode != Rendering::HdrOutputMode::Off;
}

// A boolean input only gates the mode named by the inputs applied before it, so
// `--hdr` over a game.config that names HDR10_PQ keeps HDR10_PQ. Enabling a
// request that carries no mode normalizes to Auto.
//
// "Before it" is ordering, not tier: GE_HDR gates GE_HDR_MODE from the same
// environment tier, where the mode is applied first.
//
// Returns whether the mode actually changed. A boolean that merely passed an
// inherited mode through did not author it and must not claim to be its source.
bool ApplyEnabled(HdrRequest& request, bool enabled)
{
    const Rendering::HdrOutputMode previous = request.Mode;
    request.Enabled = enabled;
    if (!enabled)
        request.Mode = Rendering::HdrOutputMode::Off;
    else if (request.Mode == Rendering::HdrOutputMode::Off)
        request.Mode = Rendering::HdrOutputMode::Auto;
    return request.Mode != previous;
}

// Only claim the next token as a value if it is not itself a flag: "--hdr-mode
// --windowed" must refuse the missing value without also swallowing --windowed.
// Same guard the editor's --debug-port applies.
const char* ValueFor(int argc, const char* const* argv, int& index)
{
    const char* next = (index + 1 < argc) ? argv[index + 1] : nullptr;
    if (!next || next[0] == '-')
        return nullptr;
    ++index;
    return next;
}

} // namespace

std::optional<std::string> PlayerHdrEnvironmentValue(const char* raw)
{
    if (!raw || raw[0] == '\0')
        return std::nullopt;
    return std::string(raw);
}

PlayerHdrEnvironment ReadPlayerHdrEnvironment()
{
    const auto read = [](const char* name) { return PlayerHdrEnvironmentValue(std::getenv(name)); };

    PlayerHdrEnvironment environment;
    environment.Mode = read("GE_HDR_MODE");
    environment.Enabled = read("GE_HDR");
    environment.BitDepth = read("GE_HDR_BIT_DEPTH");
    environment.Toggle = read("GE_PLAYER_HDR_TOGGLE");
    return environment;
}

PlayerHdrOptions ResolvePlayerHdrOptions(int argc,
                                         const char* const* argv,
                                         const PlayerHdrEnvironment& environment,
                                         const PlayerHdrProjectSettings& project)
{
    PlayerHdrOptions options;

    // Layers are applied lowest priority first, so a later one overwrites an
    // earlier one and the command line ends up governing.
    HdrRequest request{project.Enabled, project.Mode};
    options.BitDepth = project.BitDepth;

    if (environment.Mode)
    {
        if (const std::optional<Rendering::HdrOutputMode> mode =
                Rendering::TryParseHdrOutputMode(*environment.Mode))
        {
            ApplyMode(request, *mode);
            options.ModeSource = "GE_HDR_MODE";
        }
        else
        {
            Logger::Log::Warning(
                "Player: GE_HDR_MODE='{}' is not a recognized HDR output mode; ignoring it and using "
                "the game.config setting. Recognized values: {}.",
                *environment.Mode, kRecognizedModes);
        }
    }
    if (environment.Enabled)
    {
        if (const std::optional<bool> enabled = TryParseBoolean(*environment.Enabled))
        {
            if (ApplyEnabled(request, *enabled))
                options.ModeSource = "GE_HDR";
        }
        else
        {
            Logger::Log::Warning(
                "Player: GE_HDR='{}' is not a recognized boolean; ignoring it and using the "
                "game.config setting. Recognized values: {}.",
                *environment.Enabled, kRecognizedBooleans);
        }
    }
    if (environment.BitDepth)
    {
        if (const std::optional<Rendering::HdrSwapchainBitDepth> bitDepth =
                TryParseBitDepth(*environment.BitDepth))
        {
            options.BitDepth = *bitDepth;
            options.BitDepthSource = "GE_HDR_BIT_DEPTH";
        }
        else
        {
            Logger::Log::Warning(
                "Player: GE_HDR_BIT_DEPTH='{}' is not a recognized swapchain bit depth; ignoring it "
                "and using the game.config setting. Recognized values: {}.",
                *environment.BitDepth, kRecognizedBitDepths);
        }
    }
    if (environment.Toggle)
    {
        if (const std::optional<bool> toggle = TryParseBoolean(*environment.Toggle))
        {
            options.AllowRuntimeToggle = *toggle;
        }
        else
        {
            Logger::Log::Warning(
                "Player: GE_PLAYER_HDR_TOGGLE='{}' is not a recognized boolean; ignoring it and "
                "leaving the runtime HDR toggle disabled. Recognized values: {}.",
                *environment.Toggle, kRecognizedBooleans);
        }
    }

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg(argv[i]);
        if (arg == "--hdr")
        {
            if (ApplyEnabled(request, true))
                options.ModeSource = "--hdr";
        }
        else if (arg == "--no-hdr")
        {
            if (ApplyEnabled(request, false))
                options.ModeSource = "--no-hdr";
        }
        else if (arg == "--hdr-mode")
        {
            const char* value = ValueFor(argc, argv, i);
            if (!value)
            {
                Logger::Log::Error("Player: --hdr-mode requires a mode; recognized values: {}.",
                                   kRecognizedModes);
                continue;
            }
            const std::optional<Rendering::HdrOutputMode> mode = Rendering::TryParseHdrOutputMode(value);
            if (!mode)
            {
                Logger::Log::Error(
                    "Player: --hdr-mode '{}' is not a recognized HDR output mode; ignoring it and "
                    "using the {} setting. Recognized values: {}.",
                    value, options.ModeSource, kRecognizedModes);
                continue;
            }
            ApplyMode(request, *mode);
            options.ModeSource = "--hdr-mode";
        }
        else if (arg == "--hdr-bit-depth" || arg == "--hdr-swapchain-bit-depth")
        {
            const char* value = ValueFor(argc, argv, i);
            if (!value)
            {
                Logger::Log::Error("Player: {} requires a swapchain bit depth; recognized values: {}.",
                                   arg, kRecognizedBitDepths);
                continue;
            }
            const std::optional<Rendering::HdrSwapchainBitDepth> bitDepth = TryParseBitDepth(value);
            if (!bitDepth)
            {
                Logger::Log::Error(
                    "Player: {} '{}' is not a recognized swapchain bit depth; ignoring it and using "
                    "the {} setting. Recognized values: {}.",
                    arg, value, options.BitDepthSource, kRecognizedBitDepths);
                continue;
            }
            options.BitDepth = *bitDepth;
            options.BitDepthSource = arg == "--hdr-bit-depth" ? "--hdr-bit-depth"
                                                              : "--hdr-swapchain-bit-depth";
        }
        else if (arg == "--allow-hdr-toggle")
        {
            options.AllowRuntimeToggle = true;
        }
        else if (arg == "--no-hdr-toggle")
        {
            options.AllowRuntimeToggle = false;
        }
    }

    options.Enabled = request.Enabled;
    options.Mode = request.Mode;
    return options;
}

void LogPlayerHdrOptions(const PlayerHdrOptions& options)
{
    Logger::Log::Info(
        "Player: HDR output {} — mode {} (from {}), swapchain bit depth {} (from {})",
        options.Enabled ? "enabled" : "disabled",
        Rendering::HdrOutputModeToString(options.Mode),
        options.ModeSource,
        options.BitDepth == Rendering::HdrSwapchainBitDepth::Float16 ? "16" : "10",
        options.BitDepthSource);
}

} // namespace GameEngine
