#include "Sky/SkySunPathSwitch.h"

#include "Components/Rendering/SkySunPath.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Sky/SkyEnvironmentEdit.h"

namespace GameEngine::Editor
{

void SwitchSkySunPath(ECS::World* w, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                      EditorChangeNotifications* n, UndoRedoService* undo, Components::SkySunPathKind kind)
{
    if (!w)
        return;
    CommitSkyEnvironmentEdit(w, primary, extras, n, undo, "Change Sky Sun Path", [kind](Components::SkyEnvironment& sky) {
        // The first switch to Custom starts the path where the Earth path is, so the sun stays put.
        if (kind == Components::SkySunPathKind::Custom && sky.SunPath == Components::SkySunPathKind::Earth)
            Components::SkySunPath::SeedCustomPathFromEarth(sky);
        sky.SunPath = kind;
    });
    if (!n)
        return;
    n->NotifyComponentChange<Components::SkyEnvironment>(w, primary,
                                                         EditorChangeNotifications::ChangeKind::InspectorRebuild);
    for (const ECS::EntityHandle extra : extras)
        n->NotifyComponentChange<Components::SkyEnvironment>(w, extra,
                                                             EditorChangeNotifications::ChangeKind::InspectorRebuild);
}

} // namespace GameEngine::Editor
