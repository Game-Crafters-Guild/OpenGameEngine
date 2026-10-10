#pragma once

namespace GameEngine::Editor
{

/// Registers the Steam Deck target's exporter with BuildExporterRegistry. On
/// macOS the build is hosted outside BuildPipeline (a cached export template,
/// else a Docker Linux build); elsewhere BuildPipeline packages natively. Both
/// then run the optional SSH install onto the configured Deck. Main thread,
/// editor startup, once.
void RegisterSteamDeckBuildExporter();

} // namespace GameEngine::Editor
