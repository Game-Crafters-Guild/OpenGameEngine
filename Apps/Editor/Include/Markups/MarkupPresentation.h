#pragma once

#include "Components/Markup/Markup.h"
#include "ECS/ECS.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <array>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace GameEngine
{
class SceneViewController;
struct SceneViewCameraPose;
class UIElement;
}

namespace GameEngine::MarkupECS
{
struct MarkupEntry;
}

namespace GameEngine::Editor
{

// How the editor shows a mark-up's parts, shared by the panel and the inspector.

// The tag's color as 0xAARRGGBB (the vocabulary's RGBA); none for an id the vocabulary
// does not hold, which the surfaces style as "Unknown".
std::optional<uint32_t> MarkupTagArgb(uint32 tagId);
// Whether dark text reads better than light on the tag's color (WCAG contrast against
// black and white): a pill filled with the tag's color then takes the dark-text class.
bool MarkupTagReadsDarkText(uint32 tagId);
// The tag's name as the user reads it, empty for an unknown id. The vocabulary keeps the
// name the agent API uses ("InProgress"); every surface shows it with a lowercase word
// break at each lower-to-upper case step ("In progress"). Names without one show as named.
std::string MarkupTagLabel(uint32 tagId);
// The mark-up's name, or "Mark-up" when it has none. The view lives while the entity's Name
// is unchanged.
std::string_view MarkupTitleView(const ECS::World& world, ECS::EntityHandle entity);
std::string MarkupTitle(const ECS::World& world, ECS::EntityHandle entity);
// Whether the mark-up has a color of its own (Markup::Color not all zero).
bool MarkupHasOwnColor(const Components::Markup& markup);
// The color a mark-up draws in: its own (Markup::Color), else its status tag's, else the
// theme's grey for a status the vocabulary does not hold (--ui_color_markup_unknown_status).
std::array<float32, 3> MarkupDisplayRgb(const Components::Markup& markup);
// MarkupDisplayRgb as opaque 0xAARRGGBB.
uint32_t MarkupDisplayArgb(const Components::Markup& markup);
// Makes `argb` (0xAARRGGBB, as the color picker gives it) the mark-up's own color, opaque.
void SetMarkupColorArgb(Components::Markup& markup, uint32_t argb);
// "now", "5 min ago", "3 h ago", "2 d ago", then the date, for Unix seconds `then`.
std::string RelativeTimeText(int64 then, int64 now);

// "You" or "Agent", as every surface names an author.
const char* MarkupAuthorText(Components::MarkupAuthor author);
// Whether the thread entry is something someone did (created, a status change, an edit)
// rather than something someone said (a comment): an action reads as one muted line, a
// comment as a message with its text below its header, in the inspector's Thread and the
// Activity tab alike.
bool MarkupEntryIsAction(const MarkupECS::MarkupEntry& entry);
// What an action entry did, as its line reads after the author: "created it", "set it to
// In progress", "moved and resized it", "renamed it to The Docks and changed its color to" (a
// recolor that kept its color ends the line, its swatch after it: BuildMarkupEntryColorSwatch).
// A comment's text.
std::string MarkupEntryText(const MarkupECS::MarkupEntry& entry);
// The entry's kind as a CSS class both surfaces style alike: markup-entry-comment,
// markup-entry-created, markup-entry-status or markup-entry-edit.
const char* MarkupEntryKindClass(const MarkupECS::MarkupEntry& entry);
// The entry's kind marker, the same in both surfaces: a speech bubble for a comment, a plus for
// created, a pencil for an edit, a square in the status's color for a status change. Styled by MarkupsPanel.css .markup-entry-marker.
std::unique_ptr<UIElement> BuildMarkupEntryMarker(const MarkupECS::MarkupEntry& entry);
// The color an Edit entry recolored the mark-up to, as a square in the status-change marker's
// style; null for any other entry and for a recolor back to the status color.
std::unique_ptr<UIElement> BuildMarkupEntryColorSwatch(const MarkupECS::MarkupEntry& entry);

// A volume mark-up's shape in the world. The entity's WorldTransform scales, rotates and
// places the unit shape: a box's scale is its full extents, a sphere's scale.x its diameter.
struct MarkupWorldVolume
{
    Components::MarkupVolumeShape Shape = Components::MarkupVolumeShape::Box;
    Mathematics::Vector3 Center{};
    std::array<Mathematics::Vector3, 3> Axes{}; // the shape's unit axes in the world
    Mathematics::Vector3 HalfExtents{};          // along Axes; a sphere's are all its radius
    float BoundingRadius = 0.0f;                 // of the sphere around the shape
    float TopY = 0.0f;                           // the highest world Y the shape reaches
};
// The shape `matrix` (a WorldTransform's, column-major) places; none for a zero scale.
std::optional<MarkupWorldVolume> MarkupWorldVolumeFromMatrix(Components::MarkupVolumeShape shape,
                                                             const float32 (&matrix)[16]);
// The mark-up's shape in the world; none without a MarkupVolume and a WorldTransform.
std::optional<MarkupWorldVolume> ReadMarkupWorldVolume(const ECS::World& world, ECS::EntityHandle entity);
// Whether `point` lies inside `volume` or on its surface.
bool MarkupVolumeContains(const MarkupWorldVolume& volume, const Mathematics::Vector3& point);
// The tangent of the Scene View's default half vertical field of view (60 degrees), what a
// frame assumes before the view has drawn.
inline constexpr float kMarkupDefaultTanHalfFov = 0.57735027f;
// The tangent of the narrower half field of view (vertical or horizontal) of `view`'s last
// frame; kMarkupDefaultTanHalfFov for no view, before its first frame, or for an orthographic
// view (whose framing distance sets no size).
float MarkupFrameTanHalfFov(const SceneViewController* view);
// How far a frame of `volume` stands from its center so its bounding sphere fits the field
// of view whose narrower half angle has tangent `tanHalfFov`, with a 15 % margin; 0.5 m at
// least. The Mark-ups panel and markup_frame frame the volume's center from it through
// ComputeLookAtPose.
float MarkupFrameDistance(const MarkupWorldVolume& volume, float tanHalfFov);

} // namespace GameEngine::Editor
