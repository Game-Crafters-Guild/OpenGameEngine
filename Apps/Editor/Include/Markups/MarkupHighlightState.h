#pragma once

#include "ECS/ECS.h"

#include <algorithm>
#include <span>

namespace GameEngine::Editor
{

// The mark-ups the Scene View draws highlighted: the one under the pointer (its Mark-ups panel
// row, or its Hierarchy row, through the Scene View's hover), and the Scene View's selection,
// which is what the Inspector shows.
struct MarkupHighlightState
{
    ECS::EntityHandle Hovered{};
    std::span<const ECS::EntityHandle> Selected{};

    bool IsHovered(ECS::EntityHandle entity) const { return Hovered.IsValid() && entity == Hovered; }
    bool IsSelected(ECS::EntityHandle entity) const
    {
        return std::find(Selected.begin(), Selected.end(), entity) != Selected.end();
    }
};

// The hovered entity the highlight takes: the Scene View's hover (a Hierarchy row's or a
// Mark-ups panel row's), else the panel row the bridge holds (no Scene View, or one that
// dropped its hover).
inline ECS::EntityHandle MarkupHoveredEntity(ECS::EntityHandle sceneViewHovered, ECS::EntityHandle panelRow)
{
    return sceneViewHovered.IsValid() ? sceneViewHovered : panelRow;
}

} // namespace GameEngine::Editor
