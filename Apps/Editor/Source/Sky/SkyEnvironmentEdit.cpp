#include "Sky/SkyEnvironmentEdit.h"

#include "Components/Rendering/SkySunIlluminance.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Sky/SkySunLinkUndo.h"
#include "UndoRedo/UndoRedoService.h"

namespace GameEngine::Editor
{
namespace
{
std::vector<ECS::EntityHandle> SkyTargets(ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras)
{
    std::vector<ECS::EntityHandle> targets;
    targets.reserve(extras.size() + 1);
    targets.push_back(primary);
    targets.insert(targets.end(), extras.begin(), extras.end());
    return targets;
}
} // namespace

void CommitSkyEnvironmentEdit(ECS::World* w, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                              EditorChangeNotifications* n, UndoRedoService* undo, const std::string& label,
                              const std::function<void(Components::SkyEnvironment&)>& edit)
{
    if (!w)
        return;
    const std::vector<ECS::EntityHandle> targets = SkyTargets(primary, extras);
    const SkySunLinkUndo::DriveBefore driveBefore = SkySunLinkUndo::CaptureDrive(*w);

    if (!undo)
    {
        for (const ECS::EntityHandle target : targets)
        {
            const auto* comp = w->GetComponent<Components::SkyEnvironment>(target);
            if (!comp)
                continue;
            Components::SkyEnvironment updated = *comp;
            edit(updated);
            CommitComponentUpdate(w, target, n, updated);
        }
        return;
    }

    undo->BeginCompound(label);
    auto snapshot = extras.empty()
        ? InspectorDrag::MakeComponentSnapshotTarget<Components::SkyEnvironment>(w, primary, n, label)
        : InspectorDrag::MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(w, primary, extras, n, label);
    auto interactive = undo->BeginInteractiveEdit(label, std::move(snapshot));
    for (const ECS::EntityHandle target : targets)
    {
        const auto* comp = w->GetComponent<Components::SkyEnvironment>(target);
        if (!comp)
            continue;
        Components::SkyEnvironment updated = *comp;
        edit(updated);
        w->AddComponentImmediate(target, updated);
    }
    interactive.Commit();
    SkySunLinkUndo::RecordDrivenFieldsForUndo(*w, driveBefore, undo, label);
    undo->EndCompound();
}

void SwitchSkyMode(ECS::World* w, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                   EditorChangeNotifications* n, UndoRedoService* undo, Components::SkyMode mode)
{
    if (!w)
        return;
    CommitSkyEnvironmentEdit(w, primary, extras, n, undo, "Change Sky Mode",
                             [mode](Components::SkyEnvironment& sky) { sky.Mode = mode; });
    if (!n)
        return;
    for (const ECS::EntityHandle target : SkyTargets(primary, extras))
        n->NotifyComponentChange<Components::SkyEnvironment>(w, target,
                                                             EditorChangeNotifications::ChangeKind::InspectorRebuild);
}

} // namespace GameEngine::Editor
