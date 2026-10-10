#pragma once

#include "Scripting/ScriptingABI.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clip and pose-graph playback on an entity's Animator. Duration arguments
// are wall-clock seconds (not normalized to the clip's length). Clip pending commands apply
// immediately and are also consumed on the next TimelinePlaybackSystem tick.
// Graph param stamps are consumed by AnimationGraphSystem.

GE_API GE_Result GE_CDECL GE_Animator_PlayStateOnEntity(GE_Handle worldHandle,
                                                        uint64_t entityId,
                                                        const char* animationNameUtf8,
                                                        uint32_t animationNameLength);

GE_API GE_Result GE_CDECL GE_Animator_PlayStateClipOnEntity(GE_Handle worldHandle,
                                                            uint64_t entityId,
                                                            const uint8_t clipGuidBytes[16]);

GE_API GE_Result GE_CDECL GE_Animator_CrossFadeSecondsOnEntity(GE_Handle worldHandle,
                                                               uint64_t entityId,
                                                               const char* animationNameUtf8,
                                                               uint32_t animationNameLength,
                                                               float blendSeconds);

GE_API GE_Result GE_CDECL GE_Animator_CrossFadeSecondsClipOnEntity(GE_Handle worldHandle,
                                                                   uint64_t entityId,
                                                                   const uint8_t clipGuidBytes[16],
                                                                   float blendSeconds);

GE_API GE_Result GE_CDECL GE_Animator_PauseOnEntity(GE_Handle worldHandle, uint64_t entityId);
GE_API GE_Result GE_CDECL GE_Animator_StopOnEntity(GE_Handle worldHandle, uint64_t entityId);
// A non-finite time is ignored and returns GE_Result_InvalidArg.
// Each refused request logs the submitted value and entity id.
GE_API GE_Result GE_CDECL GE_Animator_SeekOnEntity(GE_Handle worldHandle, uint64_t entityId, float timeSeconds);

GE_API GE_Result GE_CDECL GE_Animator_PlayGraphOnEntity(GE_Handle worldHandle,
                                                        uint64_t entityId,
                                                        const uint8_t graphGuidBytes[16]);

GE_API GE_Result GE_CDECL GE_Animator_SetFloatOnEntity(GE_Handle worldHandle,
                                                       uint64_t entityId,
                                                       const char* nameUtf8,
                                                       uint32_t nameLength,
                                                       float value);

GE_API GE_Result GE_CDECL GE_Animator_SetBoolOnEntity(GE_Handle worldHandle,
                                                      uint64_t entityId,
                                                      const char* nameUtf8,
                                                      uint32_t nameLength,
                                                      int32_t value);

GE_API GE_Result GE_CDECL GE_Animator_SetTriggerOnEntity(GE_Handle worldHandle,
                                                         uint64_t entityId,
                                                         const char* nameUtf8,
                                                         uint32_t nameLength);

// The bytes of an event's name a GE_AnimationEventRecord holds.
#define GE_ANIMATION_EVENT_NAME_CAPACITY 64

// One animation event an Animator fired. 80 bytes, 8-byte aligned; managed code mirrors the layout
// (AnimationEventRecord).
typedef struct GE_AnimationEventRecord
{
    // The name's StringId: 64-bit FNV-1a over its UTF-8 bytes, whole name.
    uint64_t nameId;
    // The event's time on its clip (or montage), in seconds from the start: a glTF event's frame over the
    // clip's frame rate.
    float timeSeconds;
    // The name's length in bytes, whole name.
    uint32_t nameLength;
    // The name's first min(nameLength, GE_ANIMATION_EVENT_NAME_CAPACITY) bytes of UTF-8, no NUL terminator.
    char name[GE_ANIMATION_EVENT_NAME_CAPACITY];
} GE_AnimationEventRecord;

// Copies into records the events the entity's Animator fired in the last animation wave that no earlier call
// returned, in the order they fired, up to recordCapacity of them, and writes how many in *outCount. The rest
// stay for the next call, so a caller whose buffer filled calls again. Each event is returned once: a second
// call in the same frame returns the events the first left, or none. The next animation wave replaces them
// with its own, so poll every frame an event matters. Does not start the engine: an Animator no wave has
// reached has none.
//
// Main thread only: the animation wave writes the events on worker threads. A call from another thread is
// refused with GE_Result_Fail and logged once; a host that never marked a main thread (a standalone test host)
// accepts every thread. GE_Result_InvalidArg, checked first, for a null outCount, a negative recordCapacity or
// a null records with a nonzero recordCapacity; GE_Result_InvalidArg too for a null world; GE_Result_NotFound
// when the entity is not alive or has no Animator; GE_Result_Fail also for an internal error. *outCount is 0
// on every result but GE_Result_Ok.
GE_API GE_Result GE_CDECL GE_Animator_PollEventsOnEntity(GE_Handle worldHandle,
                                                         uint64_t entityId,
                                                         GE_AnimationEventRecord* records,
                                                         int32_t recordCapacity,
                                                         int32_t* outCount);

#ifdef __cplusplus
} // extern "C"
#endif
