#pragma once

// Shared prelude for the change-signaling enforcement compile-FAIL matrix
// (design §4.6 ConstEnforcement, M4a + D-SDK). Each case file defines one
// function that must NOT compile; the CMake harness builds each case as its
// own target with a WILL_FAIL ctest. CompilePassControl.cpp is the positive
// control that must keep compiling (harness-rot canary).

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"
#include "../TestComponents.h"

namespace CompileFailCase
{
using GameEngine::ECS::World;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::Read;
using GameEngine::ECS::Write;
using GameEngine::ECS::Optional;
using GameEngine::ECS::test::Position;
using GameEngine::ECS::test::Velocity;
} // namespace CompileFailCase
