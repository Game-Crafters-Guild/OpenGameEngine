#pragma once

#include "Types/StringUtils.h"

#include <cstddef>
#include <string_view>

namespace GameEngine::Components
{

// Marks an entity as the root of a scene [blueprint] instance, with enough metadata
// to re-serialize the instance back to a scene file.
//
// Stored as fixed-size buffers for ECS component constraints. A raw component write
// can fill every byte with no terminator, so read them through the accessors.
// @ge-no-add  data helper, not user-addable in the editor
// [DoNotSerialize] — SceneIO reconstructs the [blueprint] section from this metadata; reflection would emit invalid property lines.
struct SceneBlueprintInstance
{
    // Blueprint asset path as authored in the scene's [resource] section (usually relative to asset root).
    char sourcePath[260]{};

    // Optional GUID string (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) if the scene authored guid=.
    char sourceGuid[64]{};

    [[nodiscard]] std::string_view SourcePath() const { return FixedStringView(sourcePath); }
    [[nodiscard]] std::string_view SourceGuid() const { return FixedStringView(sourceGuid); }
};

} // namespace GameEngine::Components

