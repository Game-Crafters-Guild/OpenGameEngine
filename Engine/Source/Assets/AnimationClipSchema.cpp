#include "AnimationClipSchema.h"

#include "Animation/AnimationClip.h"
#include "Animation/AnimationEvent.h"
#include "Assets/ModelExtras.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{

using json = nlohmann::json;

// The one version of the clip schema this reader reads.
constexpr double kClipSchemaVersion = 1.0;
// The deepest an extras block holding a schema may nest its objects and arrays; the block itself is level 1.
constexpr int kSchemaDepthLimit = 16;
constexpr size_t kBytesPerKibibyte = 1024u;
// The most bytes of file text a refusal quotes; longer text is cut there and its length given.
constexpr size_t kQuotedTextLimit = 64u;
// How far past the clip's end an event still counts as on its last frame, in float epsilons of the end frame. The
// clip's length and rate come from 32-bit float key times, so the end frame, length x rate, carries a relative
// rounding error of up to about one float epsilon (at most 0.93 of one for clips keyed on every frame at 24 to
// 120 frames per second and up to 72 000 frames long); four allow for it.
constexpr double kLastFrameToleranceInFloatEpsilons = 4.0;
// The most that tolerance may be, in frames: below one, so an event on the frame after the end is refused however
// long the clip (four epsilons reach a whole frame at 2^21 frames).
constexpr double kLastFrameToleranceLimit = 0.5;
// A name holds no byte below the first printable character or the delete character: a NUL would end it at a
// C string boundary, and a line break would split the log line that names it.
constexpr unsigned char kFirstPrintableCharacter = 0x20u;
constexpr unsigned char kDeleteCharacter = 0x7Fu;

// `text` as a JSON string literal: quoted and escaped, on one line.
std::string JsonStringLiteral(std::string_view text)
{
    return json(std::string(text)).dump(-1, ' ', false, json::error_handler_t::replace);
}

// File text as a refusal quotes it: a JSON string literal of at most kQuotedTextLimit bytes of it, followed by
// its length when it is longer.
std::string Quoted(std::string_view text)
{
    if (text.size() <= kQuotedTextLimit)
        return JsonStringLiteral(text);
    return std::format("{}... ({} bytes)", JsonStringLiteral(text.substr(0, kQuotedTextLimit)), text.size());
}

// A pass over extras text that checks what only a parse sees: objects and arrays nested past the depth limit, and a
// key an object already holds (a parsed document keeps only the last). It stops at the first of either. The text is
// parsed into a document after it, with no parse callback: nlohmann's callback parser rescans a container each time
// an object in it closes, which is quadratic in the number of objects, while this pass and a plain parse are linear.
class ExtrasBoundsCheck final : public json::json_sax_t
{
  public:
    const std::string& Refusal() const
    {
        return m_Refusal;
    }

    bool null() override
    {
        return true;
    }
    bool boolean(bool) override
    {
        return true;
    }
    bool number_integer(number_integer_t) override
    {
        return true;
    }
    bool number_unsigned(number_unsigned_t) override
    {
        return true;
    }
    bool number_float(number_float_t, const string_t&) override
    {
        return true;
    }
    bool string(string_t&) override
    {
        return true;
    }
    bool binary(binary_t&) override
    {
        return true;
    }
    bool start_object(std::size_t) override
    {
        if (!OpenLevel())
            return false;
        m_KeysOfOpenObjects.emplace_back();
        return true;
    }
    bool key(string_t& name) override
    {
        if (m_KeysOfOpenObjects.back().insert(name).second)
            return true;
        m_Refusal = std::format("the animation's extras have the key {} twice in one object", Quoted(name));
        return false;
    }
    bool end_object() override
    {
        m_KeysOfOpenObjects.pop_back();
        --m_Depth;
        return true;
    }
    bool start_array(std::size_t) override
    {
        return OpenLevel();
    }
    bool end_array() override
    {
        --m_Depth;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override
    {
        return false;
    }

  private:
    // Enters one more level of objects and arrays; false, with the refusal, past the depth limit.
    bool OpenLevel()
    {
        if (++m_Depth <= kSchemaDepthLimit)
            return true;
        m_Refusal = std::format("the animation's extras nest objects and arrays deeper than {} levels",
                                kSchemaDepthLimit);
        return false;
    }

    int m_Depth = 0;
    std::vector<std::unordered_set<std::string>> m_KeysOfOpenObjects;
    std::string m_Refusal;
};

// The extras as a JSON document, or why they are not read.
struct BoundedExtras
{
    json Document;
    std::string Refusal;
};

// Parses `text` within the schema bounds: its size, its depth and one value per key in each object.
BoundedExtras ParseBoundedExtras(std::string_view text)
{
    BoundedExtras extras;
    if (text.size() > ModelExtras::kObjectBoundBytes)
    {
        extras.Refusal = std::format("the animation's extras hold {} bytes, and a schema is read only from extras "
                                     "of at most {} bytes ({} KiB)",
                                     text.size(), ModelExtras::kObjectBoundBytes,
                                     ModelExtras::kObjectBoundBytes / kBytesPerKibibyte);
        return extras;
    }

    ExtrasBoundsCheck check;
    if (!json::sax_parse(text.begin(), text.end(), &check))
    {
        extras.Refusal = check.Refusal().empty() ? "the animation's extras are not valid JSON" : check.Refusal();
        return extras;
    }
    extras.Document = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (extras.Document.is_discarded())
        extras.Refusal = "the animation's extras are not valid JSON";
    return extras;
}

// `value` when it is a whole number of 0 or more, written with or without a decimal point (19 or 19.0).
std::optional<double> WholeNumber(const json& value)
{
    if (!value.is_number())
        return std::nullopt;
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 || std::floor(number) != number)
        return std::nullopt;
    return number;
}

// `number` in fixed notation with at most four decimals and no trailing zeros: 10.6, 12345, 69618.9961.
std::string PlainNumber(double number)
{
    std::string text = std::format("{:.4f}", number);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.')
        text.pop_back();
    return text;
}

// True when `name` holds a byte a name must not (see kFirstPrintableCharacter).
bool HasControlCharacter(std::string_view name)
{
    return std::any_of(name.begin(), name.end(),
                       [](char character)
                       {
                           const auto byte = static_cast<unsigned char>(character);
                           return byte < kFirstPrintableCharacter || byte == kDeleteCharacter;
                       });
}

// Why `field` is not a name for `fieldPath`; empty when it is one.
std::string CheckName(const json& field, std::string_view fieldPath)
{
    if (!field.is_string() || field.get_ref<const std::string&>().empty())
        return std::format("{} is not a non-empty string", fieldPath);
    if (HasControlCharacter(field.get_ref<const std::string&>()))
        return std::format("{} holds a control character", fieldPath);
    return {};
}

// Why `clip` is not at the version this reader reads; empty when it is.
std::string CheckSchemaVersion(const json& clip)
{
    const auto version = clip.find("schemaVersion");
    if (version == clip.end())
        return std::format("clip has no schemaVersion; write \"schemaVersion\": {}", kClipSchemaVersion);
    if (WholeNumber(*version) != kClipSchemaVersion)
        return std::format("clip.schemaVersion {} is not a version this engine reads; it reads schemaVersion {}",
                           version->is_number() ? version->dump() : std::string("(not a number)"), kClipSchemaVersion);
    return {};
}

std::string ReadLoop(const json& value, Animation::AnimationClipSettings& settings)
{
    if (!value.is_boolean())
        return "clip.loop is not true or false";
    settings.Loop = value.get<bool>();
    return {};
}

std::string ReadSpeed(const json& value, Animation::AnimationClipSettings& settings)
{
    if (!value.is_number())
        return "clip.speed is not a number";
    const double speed = value.get<double>();
    // Converting a double outside the float range is undefined, so the range is checked before the conversion.
    if (!(std::abs(speed) <= std::numeric_limits<float32>::max()))
        return std::format("clip.speed {} is larger than a 32-bit float holds", value.dump());
    const float32 stored = static_cast<float32>(speed);
    if (!(stored > 0.0f))
        return speed > 0.0 ? std::format("clip.speed {} rounds to 0 as a 32-bit float", value.dump())
                           : std::format("clip.speed {} is not greater than 0", value.dump());
    settings.Speed = stored;
    return {};
}

std::string ReadRootMotion(const json& value, Animation::AnimationClipSettings& settings)
{
    if (!value.is_object())
        return "clip.rootMotion is not a JSON object";
    Animation::ClipRootMotion rootMotion;
    for (const auto& [key, field] : value.items())
    {
        if (key == "bone")
        {
            if (std::string refusal = CheckName(field, "clip.rootMotion.bone"); !refusal.empty())
                return refusal;
            rootMotion.Bone = field.get<std::string>();
        }
        else if (key == "translation" || key == "rotation")
        {
            if (!field.is_boolean())
                return std::format("clip.rootMotion.{} is not true or false", key);
            bool& part = key == "translation" ? rootMotion.Translation : rootMotion.Rotation;
            part = field.get<bool>();
        }
        else
        {
            return std::format("clip.rootMotion has the field {}; its fields are bone, translation and rotation",
                               Quoted(key));
        }
    }
    if (rootMotion.Bone.empty())
        return "clip.rootMotion names no bone";
    settings.RootMotion = std::move(rootMotion);
    return {};
}

std::string ReadTranslations(const json& value, Animation::AnimationClipSettings& settings)
{
    if (value == "absolute")
        settings.Translations = Animation::ClipTranslationMode::Absolute;
    else if (value == "restRelative")
        settings.Translations = Animation::ClipTranslationMode::RestRelative;
    else
        return "clip.translations is not \"absolute\" or \"restRelative\"";
    return {};
}

// The rate, in frames per second, at which the clip of `channels` is sampled: the key count of the channel with the
// most keys (the first of them on a tie), less one, over that channel's time span. Zero when that channel has no
// two keys apart in time.
double FramesPerSecond(const std::vector<Animation::AnimChannel>& channels)
{
    const Animation::AnimChannel* mostKeys = nullptr;
    for (const Animation::AnimChannel& channel : channels)
    {
        if (!mostKeys || channel.keys.size() > mostKeys->keys.size())
            mostKeys = &channel;
    }
    if (!mostKeys || mostKeys->keys.size() < 2u)
        return 0.0;
    const double span = static_cast<double>(mostKeys->keys.back().time) - mostKeys->keys.front().time;
    return span > 0.0 ? static_cast<double>(mostKeys->keys.size() - 1u) / span : 0.0;
}

// Reads the events of `value` into `events`, timed at `framesPerSecond` from the clip's time 0, sorted by time
// with events at one time kept in file order. An event after the clip's end is refused: it could never fire.
std::string ReadEvents(const json& value, double framesPerSecond, float32 duration,
                       Animation::AnimationEventTrack& events)
{
    if (!value.is_array())
        return "clip.events is not a JSON array";
    if (!value.empty() && framesPerSecond <= 0.0)
        return "clip.events need the clip's frame rate, and the clip's channel with the most keys has no two keys "
               "apart in time";
    const double lastFrame = static_cast<double>(duration) * framesPerSecond;
    const double lastFrameTolerance =
        std::min(lastFrame * kLastFrameToleranceInFloatEpsilons * std::numeric_limits<float32>::epsilon(),
                 kLastFrameToleranceLimit);

    std::vector<Animation::AnimationEvent> read;
    read.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index)
    {
        const json& entry = value[index];
        if (!entry.is_object())
            return std::format("clip.events[{}] is not a JSON object", index);
        std::optional<double> frame;
        Animation::AnimationEvent event;
        for (const auto& [key, field] : entry.items())
        {
            if (key == "frame")
            {
                frame = WholeNumber(field);
                if (!frame)
                    return std::format("clip.events[{}].frame is not a whole number of 0 or more", index);
            }
            else if (key == "name")
            {
                if (std::string refusal = CheckName(field, std::format("clip.events[{}].name", index));
                    !refusal.empty())
                    return refusal;
                event.Name = field.get<std::string>();
            }
            else
            {
                return std::format("clip.events[{}] has the field {}; its fields are frame and name", index,
                                   Quoted(key));
            }
        }
        if (!frame)
            return std::format("clip.events[{}] has no frame", index);
        if (event.Name.empty())
            return std::format("clip.events[{}] has no name", index);
        if (*frame > lastFrame + lastFrameTolerance)
            return std::format("clip.events[{}] ({}) is at frame {}, after the clip ends at frame {} "
                               "({} frames per second); frame 0 is the clip's start",
                               index, Quoted(event.Name), *frame, PlainNumber(lastFrame),
                               PlainNumber(framesPerSecond));
        event.Time = static_cast<float32>(*frame / framesPerSecond);
        read.push_back(std::move(event));
    }

    std::stable_sort(read.begin(), read.end(),
                     [](const Animation::AnimationEvent& left, const Animation::AnimationEvent& right)
                     { return left.Time < right.Time; });
    for (const Animation::AnimationEvent& event : read)
        events.AddEvent(event);
    return {};
}

} // namespace

std::string ReadClipSchema(std::string_view extrasText, const std::vector<Animation::AnimChannel>& channels,
                           float32 duration, Animation::AnimationClipSettings& outSettings,
                           Animation::AnimationEventTrack& outEvents)
{
    if (extrasText.empty())
        return {};
    const BoundedExtras extras = ParseBoundedExtras(extrasText);
    if (!extras.Refusal.empty())
        return extras.Refusal;
    if (!extras.Document.is_object())
        return {};
    const auto clipBlock = extras.Document.find("clip");
    if (clipBlock == extras.Document.end())
        return {};
    const json& clip = *clipBlock;
    if (!clip.is_object())
        return "clip is not a JSON object";
    if (std::string refusal = CheckSchemaVersion(clip); !refusal.empty())
        return refusal;

    Animation::AnimationClipSettings settings;
    Animation::AnimationEventTrack events;
    for (const auto& [key, value] : clip.items())
    {
        std::string refusal;
        if (key == "schemaVersion")
            continue;
        if (key == "loop")
            refusal = ReadLoop(value, settings);
        else if (key == "speed")
            refusal = ReadSpeed(value, settings);
        else if (key == "rootMotion")
            refusal = ReadRootMotion(value, settings);
        else if (key == "translations")
            refusal = ReadTranslations(value, settings);
        else if (key == "events")
            refusal = ReadEvents(value, FramesPerSecond(channels), duration, events);
        else
            refusal = std::format("clip has the field {}; its fields are schemaVersion, loop, speed, rootMotion, "
                                  "translations and events",
                                  Quoted(key));
        if (!refusal.empty())
            return refusal;
    }
    outSettings = std::move(settings);
    outEvents = std::move(events);
    return {};
}

} // namespace GameEngine
