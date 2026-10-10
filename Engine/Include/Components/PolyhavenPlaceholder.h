#pragma once

#include "Types/StringUtils.h"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace GameEngine::Components
{

// Marks an entity as a placeholder for a Polyhaven asset that is being downloaded.
// When the download completes the placeholder entity is destroyed and replaced
// with the full model hierarchy created by ModelEntityFactory.
// @ge-no-add  data helper, not user-addable in the editor
// [DoNotSerialize] — transient download placeholder; destroyed and replaced by the real model when the download completes.
struct PolyhavenPlaceholder
{
    char slug[64] = {0};       // Polyhaven asset slug (e.g. "rock_wall_04")
    char assetType[16] = {0};  // "models", "textures", or "hdris"
    uint32_t downloadId = 0;   // Correlates with PolyhavenDownloadManager entry

    // The slug up to its first null byte; a raw component write can leave none.
    [[nodiscard]] std::string_view Slug() const { return FixedStringView(slug); }
};

static_assert(std::is_trivially_copyable_v<PolyhavenPlaceholder>);
static_assert(std::is_standard_layout_v<PolyhavenPlaceholder>);

} // namespace GameEngine::Components
