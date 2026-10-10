#pragma once

#include "UI/Controls/DockPanel.h"
#include <cstddef>

namespace GameEngine {

class ScrollView;
class Label;
class UIElement;
class Button;

namespace Editor { class UndoRedoService; }

class UndoHistoryPanel : public DockPanel
{
  public:
    std::string_view DeclaredTabIconClass() const override { return "undo-icon"; }

    UndoHistoryPanel();
    ~UndoHistoryPanel() override;

    void SetUndoRedoService(Editor::UndoRedoService* undo);
    void Rebuild();

  private:
    void BuildUI();

    Editor::UndoRedoService* m_Undo = nullptr;

    ScrollView* m_ScrollView = nullptr;
    UIElement* m_List = nullptr;
    Label* m_CountLabel = nullptr;
    Button* m_BackBtn = nullptr;
    Button* m_ForwardBtn = nullptr;
};

} // namespace GameEngine
