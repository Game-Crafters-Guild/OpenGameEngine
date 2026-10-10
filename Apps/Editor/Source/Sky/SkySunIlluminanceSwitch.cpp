#include "Sky/SkySunIlluminanceSwitch.h"

#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Sky/SkyEnvironmentEdit.h"

namespace GameEngine::Editor
{
namespace
{
void RebuildInspectors(ECS::World* w, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                       EditorChangeNotifications* n)
{
    if (!n)
        return;
    n->NotifyComponentChange<Components::SkyEnvironment>(w, primary,
                                                         EditorChangeNotifications::ChangeKind::InspectorRebuild);
    for (const ECS::EntityHandle extra : extras)
        n->NotifyComponentChange<Components::SkyEnvironment>(w, extra,
                                                             EditorChangeNotifications::ChangeKind::InspectorRebuild);
}
} // namespace

void SwitchSkySunIlluminanceSource(ECS::World* w, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                                   EditorChangeNotifications* n, UndoRedoService* undo,
                                   Components::SkySunIlluminanceSource source)
{
    if (!w)
        return;
    CommitSkyEnvironmentEdit(w, primary, extras, n, undo, "Change Sun Illuminance Source",
                             [w, source](Components::SkyEnvironment& sky) {
                                 if (source == Components::SkySunIlluminanceSource::Curve &&
                                     sky.SunIlluminanceSource == Components::SkySunIlluminanceSource::Light)
                                     Components::SkySunDrive::SeedCurveIfUnauthored(
                                         sky, Components::SkySunIlluminance::ClearSunLux(*w, sky));
                                 sky.SunIlluminanceSource = source;
                             });
    RebuildInspectors(w, primary, extras, n);
}

void ReplaceSkySunIlluminanceCurve(ECS::World* w, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                                   EditorChangeNotifications* n, UndoRedoService* undo)
{
    if (!w)
        return;
    CommitSkyEnvironmentEdit(w, primary, extras, n, undo, "Replace Sun Illuminance Curve",
                             [](Components::SkyEnvironment& sky) {
                                 sky.SunIlluminanceCurve = Components::SkySunDrive::PhysicalSeedCurve(
                                     sky, Components::kClearNoonSunIlluminanceLux);
                             });
    RebuildInspectors(w, primary, extras, n);
}

} // namespace GameEngine::Editor
