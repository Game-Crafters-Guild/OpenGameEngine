#pragma once

#include "Components/Rendering/SkyEnvironment.h" // SkySunPathKind
#include "ECS/Entity.h"

#include <vector>

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// Switch the Sun path of the sky on `primary` and of every sky in `extras` as one undo step, and
// rebuild their inspectors. Every authored field of both paths keeps its value; the first switch to
// Custom, while its fields are untouched, starts it where the Earth path is
// (SkySunPath::SeedCustomPathFromEarth).
void SwitchSkySunPath(ECS::World* world, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                      EditorChangeNotifications* notifications, UndoRedoService* undo, Components::SkySunPathKind kind);

} // namespace GameEngine::Editor
