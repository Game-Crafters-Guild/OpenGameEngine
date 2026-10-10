#pragma once

#include "ECS/ECS.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"

#include <string>

namespace GameEngine
{
class Label;
class Toggle;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// The inspector header's account of the selected entity's activity. While the entity is on but
/// inactive, because an ancestor is switched off, the entity toggle is muted, its tooltip names that
/// ancestor and what to do, and a line under the header states the reason; otherwise the toggle
/// reads as usual and the line is hidden.
class InspectorEntityActivityPresenter
{
  public:
    /// The controls to present on, rebuilt with the header; null for either when there is none.
    /// The next Present draws them whatever the state.
    void Bind(Toggle* toggle, Label* reasonLine, bool multiEdit);

    /// Draws `entity`'s activity when it differs from the one last drawn, the off ancestor's name
    /// included, and returns it.
    EntityActivity Present(ECS::World& world, ECS::EntityHandle entity);

  private:
    Toggle* m_Toggle = nullptr;    // not owned
    Label* m_ReasonLine = nullptr; // not owned
    bool m_MultiEdit = false;
    bool m_Drawn = false;
    EntityActivity m_DrawnActivity{};
    std::string m_DrawnOffAncestorName;
};

} // namespace GameEngine::Editor
