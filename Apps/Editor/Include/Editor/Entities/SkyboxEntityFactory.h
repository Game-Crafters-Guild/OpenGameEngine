#pragma once

#include "ECS/Entity.h"

#include <filesystem>
#include <functional>
#include <string_view>

namespace GameEngine
{
class AssetManager;
struct EditorContext;
namespace ECS { class World; }
} // namespace GameEngine

namespace GameEngine::Editor
{

class EditorChangeNotifications;

// Whether a dropped file is an equirectangular HDRI, which a drop turns into a
// skybox entity.
bool IsHdriPath(const std::filesystem::path& path);

// Creates a Skybox entity bound to an HDRI already on disk and registered with
// the asset manager. The stored HDRIPath is project-relative, since the scene
// persists it. `sourceSlug` and `resolution` record the Polyhaven origin so the
// inspector can offer other resolutions. Returns an invalid handle when the path
// resolves to no asset.
ECS::EntityHandle CreateSkyboxEntityFromHdri(ECS::World& world,
                                             AssetManager& assets,
                                             const std::filesystem::path& hdriPath,
                                             EditorChangeNotifications* notifications,
                                             const std::function<void()>& markDirty,
                                             std::string_view sourceSlug = {},
                                             std::string_view resolution = "1k");

// Creates a Skybox entity for a Polyhaven HDRI dropped from the online browser.
//
// A downloaded file at the preferred resolution binds at once. Otherwise a
// downloaded 1k file binds now and the preferred resolution downloads and
// replaces it; with nothing on disk an empty skybox is created and bound when the
// downloads land, the 1k one first as a preview. Returns the skybox entity, or an
// invalid handle when none could be created.
ECS::EntityHandle CreateSkyboxEntityFromPolyhavenHdri(ECS::World& world,
                                                      const EditorContext& context,
                                                      std::string_view slug,
                                                      std::string_view displayName,
                                                      EditorChangeNotifications* notifications);

} // namespace GameEngine::Editor
