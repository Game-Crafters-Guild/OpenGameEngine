#include "DebugServer/ThumbnailDebugHandlers.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Thumbnails/IThumbnailProvider.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace GameEngine
{

using json = nlohmann::json;

void RegisterThumbnailDebugHandlers(EditorDebugServer& server, const std::unique_ptr<IThumbnailProvider>& provider)
{
    // generate_folder_thumbnails
    // Queues the persistent thumbnails of every model and material under a
    // folder, as the Assets panel's "Generate Thumbnails" command does.
    //   path: required folder, absolute or relative to the assets root.
    server.RegisterHandler("generate_folder_thumbnails", [&provider](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const std::string requestedPath = ctx.params.value("path", std::string{});
        if (requestedPath.empty())
            return Editor::RefuseRequest("Provide path");
        std::filesystem::path folder(requestedPath);
        if (folder.is_relative())
            folder = EngineCore::GetInstance().GetAssetManager().GetAssetRoot() / folder;
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec))
            return Editor::RefuseRequest("Not a folder: " + folder.string());
        if (!provider)
            return Editor::RefuseRequest("No thumbnail provider");
        const size_t queued = provider->GenerateFolderThumbnails(folder);
        return json{{"ok", true}, {"path", folder.string()}, {"queued", queued}}; });
}

} // namespace GameEngine
