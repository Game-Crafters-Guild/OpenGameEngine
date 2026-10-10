#pragma once

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Spline/SplineTypes.h"
#include "UndoRedo/IEditorCommand.h"

#include <string>
#include <variant>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;

// What a volume mark-up becomes: a region over closed `Knots` (world x, z) `ExtrudeHeight` tall,
// standing where the volume's base stood, at the knots' label point.
struct ConvertedRegion
{
    std::vector<Mathematics::Vector2> Knots;
    Spline::SplineType Type = Spline::SplineType::Linear;
    float32 ExtrudeHeight = 8.0f;
};

// What a volume mark-up becomes: a path through `Points` (world), standing at its first point.
struct ConvertedPath
{
    std::vector<Mathematics::Vector3> Points;
    Spline::SplineType Type = Spline::SplineType::Linear;
};

// One undo step that turns a volume mark-up into a region or a path, one way: the entity keeps
// its Markup (status, color, revision), its title, its notes and its thread, loses its
// MarkupVolume and gains the shape's spline (and a region's MarkupRegion); it moves to the shape's
// placement, unrotated and unscaled (the spline carries the shape). Undo gives the volume and its
// transform back; redo puts the same spline back. The spline the command creates lives as long as
// the command holds it applied, and is destroyed with a command that was undone.
class ConvertMarkupShapeCommand final : public IEditorCommand
{
  public:
    using Shape = std::variant<ConvertedRegion, ConvertedPath>;

    ConvertMarkupShapeCommand(std::string label, ECS::World& world, ECS::EntityHandle entity,
                              EditorChangeNotifications* notifications, Shape shape);
    ~ConvertMarkupShapeCommand() override;

    ConvertMarkupShapeCommand(const ConvertMarkupShapeCommand&) = delete;
    ConvertMarkupShapeCommand& operator=(const ConvertMarkupShapeCommand&) = delete;

    const char* GetName() const override { return m_Label.c_str(); }
    const char* GetTypeName() const override { return "ConvertMarkupShapeCommand"; }
    void Do() override;
    void Undo() override;

  private:
    // Makes the shape's parts, the first time Do runs.
    void MakeShape();
    void Announce();

    std::string m_Label;
    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    EditorChangeNotifications* m_Notifications;
    Shape m_Shape;

    // The volume as it was.
    Components::Transform m_TransformBefore{};
    Components::WorldTransform m_WorldBefore{};
    Components::MarkupVolume m_VolumeBefore{};
    // The shape as the first Do made it; a path has no MarkupRegion.
    bool m_Made = false;
    bool m_Applied = false;
    Components::Transform m_TransformAfter{};
    Components::WorldTransform m_WorldAfter{};
    Components::SplineComponent m_Spline{};
    bool m_IsRegion = false;
    Components::MarkupRegion m_Region{};
};

} // namespace GameEngine::Editor
