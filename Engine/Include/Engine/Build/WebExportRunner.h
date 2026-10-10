#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine {

struct BuildSettings;
class BuildPipeline;


/// Build the Web target (BuildPlatform::Web): run Tools/Web/export_web_player.py over the open
/// project and leave a browser-servable dist in `settings.outputDirectory`.
///
/// The Web platform does not go through BuildPipeline's native compile phases —
/// nothing is compiled here. The wasm Player is an export template built ahead
/// of time; the export stages the project's content, cooks its shaders to WGSL
/// and packs the result beside a verbatim copy of that template.
///
/// `pipeline` supplies the cancellation signal and the child-process host.
/// `onLine` receives each line of exporter output.
///
/// Returns false with `outError` set to a message that names the fix whenever
/// an input cannot be resolved (no Python, no export script, no player
/// template, no cooked shader packages, no MaterialVariantCook) or the export
/// itself fails.
/// Host-resolved inputs. Where these paths come from — an editor preference, the
/// running editor's staged assets, a CI flag — is the caller's business: resolving them
/// in here would put an Apps/ dependency inside the engine's build tooling.
struct WebExportInputs
{
    /// Explicit wasm Player template directory, or empty to search the engine tree's
    /// wasm build directories.
    std::filesystem::path playerTemplate;
    /// Root holding the staged shader packages the export cooks to WGSL.
    std::filesystem::path shaderPackagesRoot;
};

bool RunWebExport(const BuildSettings& settings,
                  BuildPipeline& pipeline,
                  const WebExportInputs& inputs,
                  const std::function<void(const std::string& line)>& onLine,
                  std::string& outError);

} // namespace GameEngine
