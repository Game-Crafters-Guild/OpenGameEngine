#pragma once

#include "AssetCore/GUID.h"
#include <memory>

namespace GameEngine
{
class AssetManager;
namespace Animation
{
class SkeletonProfile;
}

inline constexpr const char* kRuntimeHumanoidProfilePath =
    "SkeletonProfiles/HumanoidStandard.profile.json";

// The installed runtime source owns this default in development. Packaged
// games fuse it into their project manifest, retaining its published GUID.
GUID ResolveRuntimeHumanoidProfile(AssetManager& assets);
// Private snapshot: safe from model decode workers and resident-asset reloads.
std::unique_ptr<Animation::SkeletonProfile> LoadRuntimeHumanoidProfile(AssetManager& assets);
} // namespace GameEngine
