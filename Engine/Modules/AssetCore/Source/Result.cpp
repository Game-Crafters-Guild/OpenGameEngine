#include "AssetCore/Result.h"

namespace GameEngine
{

const char* ToString(AssetError e) noexcept
{
    switch (e)
    {
        case AssetError::Missing:          return "Missing";
        case AssetError::ImportFailed:     return "ImportFailed";
        case AssetError::MountUnavailable: return "MountUnavailable";
        case AssetError::Cancelled:        return "Cancelled";
    }
    return "Unknown";
}

} // namespace GameEngine
