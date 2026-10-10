#include "Engine/Build/BuildHost.h"

namespace GameEngine {

BuildHost CurrentBuildHost()
{
#if defined(_WIN32)
    return BuildHost::Windows;
#elif defined(__APPLE__)
    return BuildHost::Mac;
#else
    return BuildHost::Linux;
#endif
}

bool BuildHostCompilesTarget(BuildHost host, std::string_view platformName)
{
    switch (host)
    {
    case BuildHost::Windows:
        return platformName == "Windows";
    case BuildHost::Mac:
        return platformName == "Mac";
    case BuildHost::Linux:
        return platformName == "Linux" || platformName == "Steam";
    }
    return false;
}

std::string_view UnsupportedBuildTargetMessage(BuildHost host)
{
    switch (host)
    {
    case BuildHost::Windows:
        return "The Windows editor builds Windows games. Build Linux and Steam Deck games with the "
               "Linux x64 editor, or build a Steam Deck game here with the engine's build_deck.py "
               "script, which needs WSL2 and Docker. Build macOS games with the macOS editor.";
    case BuildHost::Mac:
        return "The macOS editor builds macOS games, and Steam Deck games from a Linux runtime "
               "template. Build Windows games with the Windows editor and Linux games with the "
               "Linux x64 editor.";
    case BuildHost::Linux:
        return "The Linux editor builds Linux and Steam Deck games. Build Windows games with the "
               "Windows editor and macOS games with the macOS editor.";
    }
    return {};
}

} // namespace GameEngine
