#pragma once

namespace GameEngine::Editor
{

/// Registers the Web target's exporter with BuildExporterRegistry: a Web build
/// runs the web export (Tools/Web/export_web_player.py over a prebuilt wasm
/// Player) instead of BuildPipeline's native compile phases. Main thread,
/// editor startup, once.
void RegisterWebBuildExporter();

} // namespace GameEngine::Editor
