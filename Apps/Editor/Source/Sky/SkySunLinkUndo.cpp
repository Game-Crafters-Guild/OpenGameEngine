#include "Sky/SkySunLinkUndo.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor::SkySunLinkUndo
{
namespace
{
// Undoing puts back the fields the light had before the drive first wrote them. Doing or redoing it
// changes nothing here: the drive writes those fields itself.
class RestoreDrivenFieldsOnUndoCommand final : public IEditorCommand
{
  public:
    RestoreDrivenFieldsOnUndoCommand(std::string name, ECS::World* world, ECS::EntityHandle light, uint8_t fields,
                                     const Components::Light& authored)
        : m_Name(std::move(name)), m_World(world), m_Light(light), m_Fields(fields),
          m_Color{authored.Color[0], authored.Color[1], authored.Color[2]}, m_Intensity(authored.Intensity)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override {}
    void Undo() override
    {
        if (!m_World || !m_World->IsValid(m_Light))
            return;
        auto* light = m_World->GetComponentForWrite<Components::Light>(m_Light);
        if (!light)
            return;
        if (m_Fields & Components::SkySunIlluminance::kDrivesColor)
        {
            for (int i = 0; i < 3; ++i)
                light->Color[i] = m_Color[i];
        }
        if (m_Fields & Components::SkySunIlluminance::kDrivesIntensity)
            light->Intensity = m_Intensity;
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Light;
    uint8_t m_Fields = 0;
    float m_Color[3];
    float m_Intensity = 0.0f;
};
} // namespace

DriveBefore CaptureDrive(ECS::World& world)
{
    const ECS::EntityHandle light = Components::SkySunIlluminance::DrivenSunLight(world);
    return {light, Components::SkySunIlluminance::DrivenLightFields(world, light)};
}

void RecordDrivenFieldsForUndo(ECS::World& world, const DriveBefore& before, UndoRedoService* undo,
                               const std::string& label)
{
    if (!undo)
        return;
    const DriveBefore now = CaptureDrive(world);
    if (!now.Light.IsValid())
        return;
    const uint8_t drivenBefore = now.Light == before.Light ? before.Fields : uint8_t{0};
    const uint8_t starting = now.Fields & ~drivenBefore &
                             (Components::SkySunIlluminance::kDrivesColor | Components::SkySunIlluminance::kDrivesIntensity);
    if (starting == 0)
        return;
    if (const auto* light = world.GetComponent<Components::Light>(now.Light))
        undo->CommitAlreadyApplied(
            std::make_unique<RestoreDrivenFieldsOnUndoCommand>(label, &world, now.Light, starting, *light));
}

GenericEditUndoRecorder BeforeGenericEdit(ECS::World& world)
{
    const DriveBefore before = CaptureDrive(world);
    return [&world, before](UndoRedoService& undo, const std::string& label) {
        RecordDrivenFieldsForUndo(world, before, &undo, label);
    };
}

} // namespace GameEngine::Editor::SkySunLinkUndo
