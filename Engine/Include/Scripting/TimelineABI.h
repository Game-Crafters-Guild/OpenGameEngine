#pragma once

#include "Scripting/ScriptingABI.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GE_Timeline_EvaluateOptions
{
    int32_t loop;
    float durationOverride;
} GE_Timeline_EvaluateOptions;

// Evaluates a timeline JSON document and writes a UTF-8 JSON evaluation result.
// Result schema mirrors Animation::TimelineEvaluationResult (valueSamples, methodEvents, audioEvents, activeClips).
// Returns bytes written (excluding null terminator) or negative GE_Result on failure.
GE_API int32_t GE_CDECL GE_Timeline_EvaluateDocumentUtf8(const char* timelineJsonUtf8,
                                                         uint32_t timelineJsonLength,
                                                         float previousTime,
                                                         float currentTime,
                                                         const GE_Timeline_EvaluateOptions* options,
                                                         char* outResultJsonUtf8,
                                                         uint32_t outBufferBytes);

GE_API GE_Result GE_CDECL GE_Timeline_GetDurationFromDocumentUtf8(const char* timelineJsonUtf8,
                                                                   uint32_t timelineJsonLength,
                                                                   float* outDurationSeconds);

// Loads a timeline asset by GUID from the active AssetManager and evaluates it.
GE_API int32_t GE_CDECL GE_Timeline_EvaluateAssetGuid(const uint8_t timelineGuidBytes[16],
                                                      float previousTime,
                                                      float currentTime,
                                                      const GE_Timeline_EvaluateOptions* options,
                                                      char* outResultJsonUtf8,
                                                      uint32_t outBufferBytes);

GE_API GE_Result GE_CDECL GE_Timeline_GetDurationAssetGuid(const uint8_t timelineGuidBytes[16],
                                                           float* outDurationSeconds);

// Drives Animator + TimelinePlaybackState on an entity (PlayableDirector-style playback).
GE_API GE_Result GE_CDECL GE_Timeline_PlayOnEntity(GE_Handle worldHandle,
                                                     uint64_t entityId,
                                                     const uint8_t timelineGuidBytes[16],
                                                     int32_t loop,
                                                     int32_t autoPlay);

GE_API GE_Result GE_CDECL GE_Timeline_PauseOnEntity(GE_Handle worldHandle, uint64_t entityId);
GE_API GE_Result GE_CDECL GE_Timeline_StopOnEntity(GE_Handle worldHandle, uint64_t entityId);
GE_API GE_Result GE_CDECL GE_Timeline_SeekOnEntity(GE_Handle worldHandle, uint64_t entityId, float timeSeconds);

GE_API GE_Result GE_CDECL GE_Timeline_RegisterMethodCallback(const char* methodNameUtf8,
                                                             uint32_t methodNameLength);

GE_API GE_Result GE_CDECL GE_Timeline_UnregisterMethodCallback(const char* methodNameUtf8,
                                                               uint32_t methodNameLength);

typedef void (GE_CDECL *GE_Timeline_ManagedMethodListenerFn)(uint64_t entityId,
                                                               const char* methodNameUtf8,
                                                               uint32_t methodNameLength,
                                                               const char* argumentsUtf8,
                                                               uint32_t argumentsLength);

GE_API void GE_CDECL GE_Timeline_SetManagedMethodListener(GE_Timeline_ManagedMethodListenerFn listener);

#ifdef __cplusplus
} // extern "C"
#endif
