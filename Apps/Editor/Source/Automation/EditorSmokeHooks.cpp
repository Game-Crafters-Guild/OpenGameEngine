#include "Automation/EditorSmokeHooks.h"

#include "Assets/PolyhavenDownloadManager.h"

#include <cstdlib>
#include <string>

namespace GameEngine::Editor::Automation
{

void RunStartupSmokeHooks(PolyhavenDownloadManager& downloads, const std::filesystem::path& assetsRoot)
{
    const char* smoke = std::getenv("GE_SMOKE_POLYHAVEN");
    if (!smoke)
        return;

    const std::string spec(smoke);
    const size_t colon = spec.find(':');
    const std::string slug = spec.substr(0, colon);
    const std::string type = colon == std::string::npos ? "textures" : spec.substr(colon + 1);
    if (slug.empty())
        return;
    downloads.StartEarlyDownload(slug, type, assetsRoot);
}

} // namespace GameEngine::Editor::Automation
