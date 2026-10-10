#pragma once

#include <string_view>

namespace GameEngine {

/// The operating system the editor runs on. It decides which targets
/// BuildPipeline compiles itself.
enum class BuildHost
{
    Windows,
    Mac,
    Linux,
};

/// The host this binary was compiled for.
BuildHost CurrentBuildHost();

/// Whether `host` compiles and packages the target `platformName` itself.
/// A build from a Linux runtime template (BuildSettings::prebuiltPlayerDirectory)
/// is checked separately and does not go through this table.
bool BuildHostCompilesTarget(BuildHost host, std::string_view platformName);

/// The error shown when `host` is asked for a target it cannot build: what the
/// editor on this host builds, and how to build the other targets.
std::string_view UnsupportedBuildTargetMessage(BuildHost host);

} // namespace GameEngine
