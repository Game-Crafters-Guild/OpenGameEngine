#pragma once

// Phase 0b shim: AnimationClip moved to GameEngine::Animation. The legacy
// `GameEngine::AnimationClip` (and the supporting enums + structs) remain
// reachable via using-aliases below so existing callsites compile unchanged.
// New code should use the GameEngine::Animation namespace directly.

#include "Animation/AnimationClip.h"

namespace GameEngine
{

using AnimPath          = ::GameEngine::Animation::AnimPath;
using AnimInterp        = ::GameEngine::Animation::AnimInterp;
using AnimTangentType   = ::GameEngine::Animation::AnimTangentType;
using AnimExtrapolation = ::GameEngine::Animation::AnimExtrapolation;
using AnimKeyframe      = ::GameEngine::Animation::AnimKeyframe;
using AnimChannel       = ::GameEngine::Animation::AnimChannel;
using AnimationClip     = ::GameEngine::Animation::AnimationClip;

} // namespace GameEngine
