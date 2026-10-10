#pragma once

#include "Types/StringUtils.h"

#include <string_view>

namespace GameEngine::Components
{

// Marks an entity as the root of a scene [subscene] instance, with enough metadata
// to re-serialize the instance back to a scene file.
// @ge-no-add  data helper, not user-addable in the editor
// [DoNotSerialize] — SceneIO reconstructs the [subscene] section from this metadata; reflection would emit invalid property lines.
struct SceneSubsceneInstance
{
    char sourcePath[260]{};
    char sourceGuid[64]{};

    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float offsetZ = 0.0f;
    bool hasOffset = false;

    // The path and guid buffers can be full with no terminator after a raw component write.
    [[nodiscard]] std::string_view SourcePath() const { return FixedStringView(sourcePath); }
    [[nodiscard]] std::string_view SourceGuid() const { return FixedStringView(sourceGuid); }
};

} // namespace GameEngine::Components

