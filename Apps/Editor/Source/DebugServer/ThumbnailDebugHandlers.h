#pragma once

#include <memory>

namespace GameEngine
{

class EditorDebugServer;
class IThumbnailProvider;

// generate_folder_thumbnails: queues the persistent thumbnails of a folder, as
// the Assets panel's "Generate Thumbnails" command does. `provider` is the
// editor's owner of the thumbnail provider, read when a request arrives.
void RegisterThumbnailDebugHandlers(EditorDebugServer& server, const std::unique_ptr<IThumbnailProvider>& provider);

} // namespace GameEngine
