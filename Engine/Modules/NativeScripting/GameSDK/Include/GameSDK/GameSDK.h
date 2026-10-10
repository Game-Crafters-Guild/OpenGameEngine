#pragma once

// GameSDK.h — umbrella header for native user scripts.
//
// Two ways to declare a hot-reloadable component:
//
//   // 1. No boilerplate — inherit the tag base IN A HEADER (.h/.hpp); the build scanner
//   //    detects it there and generates the registration. (A struct inheriting the tag in
//   //    a .cpp is NOT registered — the scanner only reflects headers; you'll get a warning.)
//   struct Health : ECS::ComponentBase { float Current = 100.0f; float Max = 100.0f; };
//
//   // 2. Explicit macro — when you want a stamped, rename-stable GUID (works in a .cpp too):
//   struct Health { float Current = 100.0f; float Max = 100.0f; };
//   GE_REGISTER_COMPONENT(Health, Current, Max);
//
// The four required user-module ABI exports are supplied automatically by the SDK
// (GameSDK/UserModuleEntry.cpp, which the generated build compiles into your DLL),
// so you never write them. You link the engine import lib and call the engine's
// real C++ API directly.
//
// For the no-boilerplate path you only need the lighter <GameSDK/Component.h> (the tag
// base + the ECS alias) — this umbrella additionally pulls in the registration macros.
//
// Registration uses the PCH-light ComponentRegistrationLite.h: user components register a
// type-erased blob handler + a default-bytes factory (added via World::SetComponentBytesImmediate)
// rather than instantiating the World template surface. This keeps this umbrella — and thus
// the user DLL's precompiled header — free of ECSTemplates.h, which is the dominant compile
// cost. The scanner-generated TU includes the same header, so there is a single definition.

#include "Component.h"                            // ECS::ComponentBase tag + ECS alias (light)
#include "Components/ComponentRegistrationLite.h" // GE_REGISTER_COMPONENT / GE_REFLECT (type-erased)
