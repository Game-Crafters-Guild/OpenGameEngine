#pragma once

#include <filesystem>
#include <string>

namespace GameEngine::WebPlayer
{

/// The dist identity a loaded manifest declares.
struct WebDistInfo
{
    std::string ProjectName;
    std::string EntryScene;
};

/// Fetch this page's export dist and unpack it into `root`.
///
/// The wasm Player bakes no content at link time: it reads player-manifest.json
/// from the directory it was served from, then the packs the manifest names,
/// so one binary serves any exported project. The transfer is synchronous —
/// ASYNCIFY unwinds the caller while the browser runs the requests — which is
/// what lets this run to completion before the engine boots.
///
/// Returns false with a diagnosis in `outError` if the dist is absent,
/// truncated, or built by tools of a different schema version.
bool LoadWebDist(const std::filesystem::path& root, WebDistInfo& outInfo, std::string& outError);

} // namespace GameEngine::WebPlayer
