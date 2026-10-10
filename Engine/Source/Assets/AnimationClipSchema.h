#pragma once

#include "Types/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Animation
{
struct AnimChannel;
struct AnimationClipSettings;
class AnimationEventTrack;
} // namespace GameEngine::Animation

namespace GameEngine
{

/**
 * @brief Reads the clip schema from `extrasText`, the extras JSON of a glTF animation, for the clip whose
 *        channels are `channels` and which lasts `duration` seconds.
 *
 * The schema is the object under the extras' "clip" key at schemaVersion 1: "loop", "speed", "rootMotion",
 * "translations" and "events", each optional. An event's "frame" counts from the clip's time 0 at the rate the
 * clip is sampled. The extras text is attacker-controlled, so it is read only within the schema bounds: at most
 * ModelExtras::kObjectBoundBytes of text, nested at most 16 levels deep, with no key twice in one object, in time
 * linear in the text. A block that is read replaces `outSettings` and `outEvents`; a refused block sets neither.
 *
 * @return Why the block was refused, naming the field at fault; empty when the block was read or the extras
 *         hold no "clip" key.
 */
std::string ReadClipSchema(std::string_view extrasText, const std::vector<Animation::AnimChannel>& channels,
                           float32 duration, Animation::AnimationClipSettings& outSettings,
                           Animation::AnimationEventTrack& outEvents);

} // namespace GameEngine
