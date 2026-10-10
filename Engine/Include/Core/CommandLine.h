#pragma once

#include <string>
#include <string_view>
#include <optional>

namespace GameEngine {

struct ApplicationConfig; // fwd-declare to avoid heavy includes

struct EngineArgs {
    bool DisableClr = false;
    std::optional<std::string> AssetDirectory; // if set, overrides config
};

// Parse engine-specific command-line options.
// Supported flags:
//   --engine-disable-clr[=true|false]
//   --engine-assets=<path>
EngineArgs ParseEngineArgs(int argc, char** argv);

// Apply parsed args to an ApplicationConfig instance.
void ApplyEngineArgsToConfig(const EngineArgs& args, ApplicationConfig& cfg);

} // namespace GameEngine

