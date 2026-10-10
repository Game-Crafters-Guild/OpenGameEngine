#include "Engine/Build/BuildPlatforms.h"

#include <algorithm>
#include <array>

namespace GameEngine {
namespace {

constexpr std::array<BuildPlatformInfo, 7> kBuildPlatforms = {{
    {"Windows", "Windows", "1.0", true, true, "platform-row"},
    {"Nintendo", "Nintendo", ".", false, false, "platform-row"},
    {"Sony", "Sony", ".", false, false, "platform-row"},
    {"Steam", "Steam Deck", ".", false, true, "platform-row"},
    {"Linux", "Linux", ".", false, true, "platform-linux-row"},
    {"Mac", "Mac", ".", false, true, "platform-row"},
    {kWebBuildPlatformName, "Web", ".", false, true, "platform-row"},
}};

} // namespace

std::span<const BuildPlatformInfo> GetBuildPlatforms()
{
    return kBuildPlatforms;
}

const BuildPlatformInfo* FindBuildPlatform(std::string_view name)
{
    const auto it = std::find_if(kBuildPlatforms.begin(), kBuildPlatforms.end(),
                                 [name](const BuildPlatformInfo& p) { return p.Name == name; });
    return it != kBuildPlatforms.end() ? &*it : nullptr;
}

std::size_t GetBuildPlatformIndex(std::string_view name)
{
    const BuildPlatformInfo* platform = FindBuildPlatform(name);
    return platform != nullptr ? static_cast<std::size_t>(platform - kBuildPlatforms.data())
                               : static_cast<std::size_t>(-1);
}

std::string GetBuildPlatformDisplayName(std::string_view name)
{
    const BuildPlatformInfo* platform = FindBuildPlatform(name);
    return std::string(platform != nullptr ? platform->DisplayName : name);
}

} // namespace GameEngine
