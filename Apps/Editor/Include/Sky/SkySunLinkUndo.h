#pragma once

#include "ECS/ECS.h"
#include "Editor/Entities/EditorComponentTraits.h"

#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class UndoRedoService;

// A sky that starts driving a field of a light overwrites it on its next frame: the colour, and under
// the sky's illuminance curve the Intensity. Undoing the edit that started the drive must hand the
// light back the values its author gave it, which the sky system cannot know when the undo lands, so
// the edit records them in its own undo step.
//
// Ending a drive needs nothing here: the sky system hands back every field it stops driving itself,
// whatever ended the drive (SkyEnvironmentSystem).
namespace SkySunLinkUndo
{

// Which light the sky drives and which of its fields (SkySunIlluminance::DrivenLightFields), taken
// before an edit that can start a drive.
struct DriveBefore
{
    ECS::EntityHandle Light;
    uint8_t Fields = 0;
};

DriveBefore CaptureDrive(ECS::World& world);

// After an edit that can start a drive, inside the edit's undo step: every field of the light the sky
// system drives now that it did not drive in `before` is recorded as the light holds it now, before
// the drive writes it, so undoing the edit puts it back. Does nothing without an undo service.
void RecordDrivenFieldsForUndo(ECS::World& world, const DriveBefore& before, UndoRedoService* undo,
                               const std::string& label);

// The same record for the edits editor code makes without knowing the sky (a component's enable
// toggle or removal, the sky's creation): the Sky Environment's
// EditorComponentTraits::BeforeGenericEdit, registered by RegisterSkyEnvironmentComponentTraits.
GenericEditUndoRecorder BeforeGenericEdit(ECS::World& world);

} // namespace SkySunLinkUndo
} // namespace GameEngine::Editor
