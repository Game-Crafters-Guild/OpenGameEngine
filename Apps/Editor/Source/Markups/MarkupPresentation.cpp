#include "Markups/MarkupPresentation.h"

#include "Components/Name.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "MarkupECS/MarkupService.h"
#include "SceneViewController.h"
#include "Types/Color.h"
#include "Types/ColorUtils.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <optional>
#include <vector>

namespace GameEngine::Editor
{

namespace
{
// WCAG 2 contrast: (L1 + 0.05) / (L2 + 0.05) on relative luminance.
constexpr float32 kContrastOffset = 0.05f;
constexpr int64 kMinute = 60;
constexpr int64 kHour = 60 * kMinute;
constexpr int64 kDay = 24 * kHour;
constexpr int64 kWeek = 7 * kDay;
// A frame leaves this much room around the bounding sphere, and stands at least 0.5 m off.
constexpr float kFrameMargin = 1.15f;
constexpr float kMinFrameDistance = 0.5f;
// A field of view narrower than about 0.1 degree frames as if it were that wide.
constexpr float kMinTanHalfFov = 1.0e-3f;
// --ui_color_markup_unknown_status (tokens.css), for a status the vocabulary does not hold.
constexpr float32 kUnknownStatusRgb[4] = {0.5f, 0.5f, 0.5f, 1.0f};

uint32_t ChannelByte(float32 channel)
{
    return static_cast<uint32_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
}
} // namespace

std::optional<uint32_t> MarkupTagArgb(uint32 tagId)
{
    const auto* service = MarkupECS::MarkupService::TryGet();
    const MarkupECS::MarkupTag* tag = service ? service->GetTag(tagId) : nullptr;
    if (!tag)
        return std::nullopt;
    return (ChannelByte(tag->Color[3]) << 24) | (ChannelByte(tag->Color[0]) << 16) | (ChannelByte(tag->Color[1]) << 8) |
           ChannelByte(tag->Color[2]);
}

bool MarkupTagReadsDarkText(uint32 tagId)
{
    const auto* service = MarkupECS::MarkupService::TryGet();
    const MarkupECS::MarkupTag* tag = service ? service->GetTag(tagId) : nullptr;
    if (!tag)
        return false;
    const ColorLinear linear = ColorSRGB(tag->Color[0], tag->Color[1], tag->Color[2]).ToLinear();
    const float32 rgb[3] = {linear.r, linear.g, linear.b};
    const float32 luminance = ColorUtils::LinearRec709Luminance(rgb);
    const float32 againstBlack = (luminance + kContrastOffset) / kContrastOffset;
    const float32 againstWhite = (1.0f + kContrastOffset) / (luminance + kContrastOffset);
    return againstBlack >= againstWhite;
}

std::string MarkupTagLabel(uint32 tagId)
{
    const auto* service = MarkupECS::MarkupService::TryGet();
    const MarkupECS::MarkupTag* tag = service ? service->GetTag(tagId) : nullptr;
    if (!tag)
        return std::string();
    std::string label;
    for (size_t i = 0; i < tag->Name.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(tag->Name[i]);
        if (i > 0 && std::isupper(c) && std::islower(static_cast<unsigned char>(tag->Name[i - 1])))
        {
            label.push_back(' ');
            label.push_back(static_cast<char>(std::tolower(c)));
            continue;
        }
        label.push_back(static_cast<char>(c));
    }
    return label;
}

std::string_view MarkupTitleView(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* name = world.GetComponent<Components::Name>(entity);
    return name && !name->View().empty() ? name->View() : std::string_view("Mark-up");
}

std::string MarkupTitle(const ECS::World& world, ECS::EntityHandle entity)
{
    return std::string(MarkupTitleView(world, entity));
}

bool MarkupHasOwnColor(const Components::Markup& markup)
{
    return markup.Color[0] != 0.0f || markup.Color[1] != 0.0f || markup.Color[2] != 0.0f || markup.Color[3] != 0.0f;
}

std::array<float32, 3> MarkupDisplayRgb(const Components::Markup& markup)
{
    const float32* source = markup.Color;
    if (!MarkupHasOwnColor(markup))
    {
        const auto* service = MarkupECS::MarkupService::TryGet();
        const MarkupECS::MarkupTag* tag = service ? service->GetTag(markup.Status) : nullptr;
        source = tag ? tag->Color : kUnknownStatusRgb;
    }
    return {std::clamp(source[0], 0.0f, 1.0f), std::clamp(source[1], 0.0f, 1.0f), std::clamp(source[2], 0.0f, 1.0f)};
}

uint32_t MarkupDisplayArgb(const Components::Markup& markup)
{
    const std::array<float32, 3> rgb = MarkupDisplayRgb(markup);
    return 0xFF000000u | (ChannelByte(rgb[0]) << 16) | (ChannelByte(rgb[1]) << 8) | ChannelByte(rgb[2]);
}

void SetMarkupColorArgb(Components::Markup& markup, uint32_t argb)
{
    markup.Color[0] = static_cast<float32>((argb >> 16) & 0xFFu) / 255.0f;
    markup.Color[1] = static_cast<float32>((argb >> 8) & 0xFFu) / 255.0f;
    markup.Color[2] = static_cast<float32>(argb & 0xFFu) / 255.0f;
    markup.Color[3] = 1.0f;
}

std::string RelativeTimeText(int64 then, int64 now)
{
    const int64 age = std::max<int64>(0, now - then);
    if (age < kMinute)
        return "now";
    if (age < kHour)
        return std::to_string(age / kMinute) + " min ago";
    if (age < kDay)
        return std::to_string(age / kHour) + " h ago";
    if (age < kWeek)
        return std::to_string(age / kDay) + " d ago";
    const std::time_t time = static_cast<std::time_t>(then);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[16];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
    return text;
}

const char* MarkupAuthorText(Components::MarkupAuthor author)
{
    return author == Components::MarkupAuthor::Agent ? "Agent" : "You";
}

bool MarkupEntryIsAction(const MarkupECS::MarkupEntry& entry)
{
    return entry.Kind == MarkupECS::MarkupEntryKind::Created || entry.Kind == MarkupECS::MarkupEntryKind::StatusChange ||
           entry.Kind == MarkupECS::MarkupEntryKind::Edit;
}

namespace
{

// Joins phrases as a sentence lists them: "a", "a and b", "a, b and c".
std::string JoinPhrases(const std::vector<std::string>& phrases)
{
    std::string joined;
    for (std::size_t index = 0; index < phrases.size(); ++index)
    {
        if (index > 0)
            joined += index + 1 == phrases.size() ? " and " : ", ";
        joined += phrases[index];
    }
    return joined;
}

// "moved and resized it and renamed it to The Docks": the verbs that take the mark-up as their
// object share one "it"; a rename names the name it gave (`newName`, when the entry kept one);
// the rest name what they changed.
std::string EditText(uint8 changes, const std::string& newName, bool namesColor)
{
    using MarkupECS::MarkupEditChange;
    std::vector<std::string> verbs;
    std::vector<std::string> others;
    for (const MarkupEditChange change : MarkupECS::kMarkupEditChangeOrder)
    {
        if ((changes & static_cast<uint8>(change)) == 0)
            continue;
        switch (change)
        {
        case MarkupEditChange::Renamed:
            others.emplace_back(newName.empty() ? std::string("renamed it") : "renamed it to " + newName);
            break;
        case MarkupEditChange::Converted: others.emplace_back("converted it to a region"); break;
        case MarkupEditChange::Reshaped: others.emplace_back("changed its shape"); break;
        case MarkupEditChange::Recolored: break; // last, so its swatch ends the line
        case MarkupEditChange::Described: others.emplace_back("edited the description"); break;
        default: verbs.emplace_back(MarkupECS::MarkupEditChangeName(change)); break;
        }
    }
    std::vector<std::string> phrases;
    if (!verbs.empty())
        phrases.push_back(JoinPhrases(verbs) + " it");
    phrases.insert(phrases.end(), others.begin(), others.end());
    if ((changes & static_cast<uint8>(MarkupEditChange::Recolored)) != 0)
        phrases.emplace_back(namesColor ? "changed its color to" : "changed its color");
    return phrases.empty() ? std::string("edited it") : JoinPhrases(phrases);
}

} // namespace

std::string MarkupEntryText(const MarkupECS::MarkupEntry& entry)
{
    switch (entry.Kind)
    {
    case MarkupECS::MarkupEntryKind::Created: return "created it";
    case MarkupECS::MarkupEntryKind::StatusChange: return "set it to " + MarkupTagLabel(entry.Status);
    case MarkupECS::MarkupEntryKind::Edit: return EditText(entry.Changes, entry.Text, entry.ColorArgb != 0);
    default: return entry.Text;
    }
}

std::unique_ptr<UIElement> BuildMarkupEntryMarker(const MarkupECS::MarkupEntry& entry)
{
    auto marker = std::make_unique<UIElement>();
    marker->AddClass("markup-entry-marker");
    marker->AddClass(MarkupEntryKindClass(entry));
    if (entry.Kind == MarkupECS::MarkupEntryKind::StatusChange)
    {
        if (const std::optional<uint32_t> argb = MarkupTagArgb(entry.Status))
            marker->Overrides().Set(Style::BackgroundColor, *argb);
    }
    return marker;
}

std::unique_ptr<UIElement> BuildMarkupEntryColorSwatch(const MarkupECS::MarkupEntry& entry)
{
    if (entry.Kind != MarkupECS::MarkupEntryKind::Edit || entry.ColorArgb == 0)
        return nullptr;
    auto swatch = std::make_unique<UIElement>();
    swatch->AddClass("markup-entry-marker");
    swatch->AddClass("markup-entry-status");
    swatch->AddClass("markup-entry-color");
    swatch->Overrides().Set(Style::BackgroundColor, entry.ColorArgb);
    return swatch;
}

const char* MarkupEntryKindClass(const MarkupECS::MarkupEntry& entry)
{
    switch (entry.Kind)
    {
    case MarkupECS::MarkupEntryKind::Created: return "markup-entry-created";
    case MarkupECS::MarkupEntryKind::StatusChange: return "markup-entry-status";
    case MarkupECS::MarkupEntryKind::Edit: return "markup-entry-edit";
    default: return "markup-entry-comment";
    }
}

std::optional<MarkupWorldVolume> MarkupWorldVolumeFromMatrix(Components::MarkupVolumeShape shape,
                                                             const float32 (&matrix)[16])
{
    using Mathematics::Vector3;
    const std::array<Vector3, 3> columns = {Vector3(matrix[0], matrix[1], matrix[2]),
                                            Vector3(matrix[4], matrix[5], matrix[6]),
                                            Vector3(matrix[8], matrix[9], matrix[10])};
    MarkupWorldVolume volume;
    volume.Shape = shape;
    volume.Center = Vector3(matrix[12], matrix[13], matrix[14]);
    float scale[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        scale[axis] = columns[axis].Length();
        if (scale[axis] <= 0.0f)
            return std::nullopt;
        volume.Axes[axis] = columns[axis] * (1.0f / scale[axis]);
    }
    if (shape == Components::MarkupVolumeShape::Sphere)
    {
        const float radius = scale[0] * 0.5f;
        volume.HalfExtents = Vector3(radius, radius, radius);
        volume.BoundingRadius = radius;
        volume.TopY = volume.Center.y + radius;
        return volume;
    }
    volume.HalfExtents = Vector3(scale[0], scale[1], scale[2]) * 0.5f;
    volume.BoundingRadius = volume.HalfExtents.Length();
    volume.TopY = volume.Center.y + std::abs(volume.Axes[0].y) * volume.HalfExtents.x +
                  std::abs(volume.Axes[1].y) * volume.HalfExtents.y + std::abs(volume.Axes[2].y) * volume.HalfExtents.z;
    return volume;
}

std::optional<MarkupWorldVolume> ReadMarkupWorldVolume(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* volume = world.GetComponent<Components::MarkupVolume>(entity);
    const auto* transform = world.GetComponent<Components::WorldTransform>(entity);
    if (!volume || !transform)
        return std::nullopt;
    return MarkupWorldVolumeFromMatrix(volume->Shape, transform->matrix);
}

bool MarkupVolumeContains(const MarkupWorldVolume& volume, const Mathematics::Vector3& point)
{
    using Mathematics::Vector3;
    const Vector3 offset = point - volume.Center;
    if (volume.Shape == Components::MarkupVolumeShape::Sphere)
        return offset.LengthSquared() <= volume.BoundingRadius * volume.BoundingRadius;
    const float halfExtents[3] = {volume.HalfExtents.x, volume.HalfExtents.y, volume.HalfExtents.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(Vector3::Dot(offset, volume.Axes[axis])) > halfExtents[axis])
            return false;
    }
    return true;
}

float MarkupFrameTanHalfFov(const SceneViewController* view)
{
    float projection[16];
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // An orthographic projection's w row is (0, 0, 0, 1); a perspective one's has no w term.
    if (!view || !view->GetLastFrameProjection(projection, width, height) || projection[15] != 0.0f ||
        projection[0] == 0.0f || projection[5] == 0.0f)
        return kMarkupDefaultTanHalfFov;
    // The diagonal holds the cotangents of the half fields of view; the larger is the narrower.
    return 1.0f / std::max(std::abs(projection[0]), std::abs(projection[5]));
}

float MarkupFrameDistance(const MarkupWorldVolume& volume, float tanHalfFov)
{
    const float sinHalfFov = std::sin(std::atan(std::max(tanHalfFov, kMinTanHalfFov)));
    return std::max(volume.BoundingRadius / sinHalfFov * kFrameMargin, kMinFrameDistance);
}

} // namespace GameEngine::Editor
