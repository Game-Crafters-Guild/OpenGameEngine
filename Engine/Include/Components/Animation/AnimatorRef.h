#pragma once

#include "Types/Types.h"

#include <cmath>

namespace GameEngine { namespace Components {

// Lightweight ECS-friendly animator. Refers to a clip by index in an Engine-owned store.
// Playback pose is a single incoming clip, plus an optional two-slot crossfade
// from PrevClipIndex. No heap; blend state lives in these fields.
struct AnimatorRef {
    // The playback state the Animator drives, not a feature: it has no off state of its own.
    static constexpr bool NotToggleable = true;

    uint32 ClipIndex = 0;   // [DoNotSerialize] ClipStore handle resolved from Animator.clipGuid at load; 0 = invalid
    float32 Time = 0.0f;    // seconds
    float32 Speed = 1.0f;   // playback speed (>=0)
    float32 BlendTime = 0.0f;
    float32 BlendDuration = 0.0f;
    float32 SectionStart = 0.0f;
    float32 SectionEnd = 0.0f;
    uint32 Flags = 0;       // bit0: loop, bit1: paused
    uint32 PrevClipIndex = 0; // [DoNotSerialize] outgoing clip while BlendTime < BlendDuration
    float32 PrevTime = 0.0f;

    static constexpr uint32 kFlag_Loop = 1u << 0;
    static constexpr uint32 kFlag_Paused = 1u << 1;
    static constexpr uint32 kFlag_Section = 1u << 2;

    bool IsLooping() const { return (Flags & kFlag_Loop) != 0; }
    bool IsPaused() const { return (Flags & kFlag_Paused) != 0; }
    bool IsSectionPlayback() const { return (Flags & kFlag_Section) != 0; }

    bool IsBlending() const
    {
        return PrevClipIndex != 0 && BlendDuration > 0.0f && BlendTime < BlendDuration;
    }

    // 0 = fully previous, 1 = fully incoming. 1 when not blending.
    float32 BlendAlpha() const
    {
        if (!IsBlending())
            return 1.0f;
        return BlendTime / BlendDuration;
    }

    void ClearBlend()
    {
        PrevClipIndex = 0;
        PrevTime = 0.0f;
        BlendTime = 0.0f;
        BlendDuration = 0.0f;
    }

    // Switch the incoming clip. blendSeconds > 0 crossfades from the clip
    // already playing (if any). Incoming time starts at 0. A call for the
    // clip already incoming is a no-op. A call for the outgoing clip
    // reverses the remaining fade instead of restarting.
    // Two-slot only: a third clip mid-fade stashes the current incoming
    // clip, not a frozen lerp of the previous pair.
    void SetAnimation(uint32 clipIndex, float32 blendSeconds = 0.0f)
    {
        if (clipIndex == 0)
        {
            ClipIndex = 0;
            Time = 0.0f;
            ClearBlend();
            return;
        }
        if (clipIndex == ClipIndex)
            return;
        if (IsBlending() && clipIndex == PrevClipIndex)
        {
            const uint32 outgoingClip = ClipIndex;
            const float32 outgoingTime = Time;
            ClipIndex = PrevClipIndex;
            Time = PrevTime;
            PrevClipIndex = outgoingClip;
            PrevTime = outgoingTime;
            BlendTime = BlendDuration - BlendTime;
            return;
        }
        const bool crossfade = std::isfinite(blendSeconds) && blendSeconds > 0.0f && ClipIndex != 0;
        if (crossfade)
        {
            PrevClipIndex = ClipIndex;
            PrevTime = Time;
            BlendTime = 0.0f;
            BlendDuration = blendSeconds;
        }
        else
        {
            ClearBlend();
        }
        ClipIndex = clipIndex;
        Time = 0.0f;
    }

    // Force-state names for gameplay. Duration is wall-clock seconds.
    // PlayState is a hard cut; CrossFadeSeconds(clip, 0) is the same cut.
    void PlayState(uint32 clipIndex)
    {
        SetAnimation(clipIndex, 0.0f);
    }

    void CrossFadeSeconds(uint32 clipIndex, float32 blendSeconds)
    {
        SetAnimation(clipIndex, blendSeconds);
    }
};

} } // namespace GameEngine::Components
