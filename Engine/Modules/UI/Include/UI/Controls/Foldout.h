#pragma once

#include "UI/UIElement.h"
#include <functional>
#include <string>

namespace GameEngine
{

class Label;

// Foldout control that displays a clickable header with optional icon and chevron.
// Clicking toggles visibility of child content between expanded and collapsed states.
// Follows Material Design / Bootstrap patterns with chevron indicator.
class Foldout : public UIElement
{
  public:
    Foldout();
    ~Foldout() override = default;

    // Title text displayed in the header
    void SetTitle(const std::string& title);
    const std::string& GetTitle() const { return m_Title; }

    // Optional icon class applied to the icon element (e.g., for font icons)
    void SetIconClass(const std::string& iconClass);
    const std::string& GetIconClass() const { return m_IconClass; }

    // Expanded/collapsed state
    void SetExpanded(bool expanded);
    bool IsExpanded() const { return m_Expanded; }
    void Expand() { SetExpanded(true); }
    void Collapse() { SetExpanded(false); }
    void Toggle();

    // Callback when expanded state changes
    using ExpandedChangedHandler = std::function<void(Foldout&, bool expanded)>;
    void SetOnExpandedChanged(ExpandedChangedHandler handler) { m_OnExpandedChanged = std::move(handler); }

    // Lazy content builder: invoked exactly once on the first expansion.
    // Use to defer expensive UI construction (e.g. asset loads, large
    // sub-trees) for foldouts that the user may never open. The builder
    // receives the content container and should populate it as if it were
    // building children directly.
    using LazyContentBuilder = std::function<void(UIElement* contentContainer)>;
    void SetLazyContentBuilder(LazyContentBuilder builder) { m_LazyContentBuilder = std::move(builder); }

    // Override to handle click events on header
    void OnEvent(UIEvent& e) override;

    // Override child management to redirect user-provided children to the content container
    void AddChild(std::unique_ptr<UIElement> child) override;
    void RemoveChild(UIElement* child) override;
    std::unique_ptr<UIElement> TakeChild(UIElement* child) override;

    // Content container where child elements should be placed
    UIElement* GetContentContainer() const { return m_ContentContainer; }

    // Header element (for adding custom widgets like close buttons)
    UIElement* GetHeader() const { return m_Header; }

  protected:
    void EnsureStructure();
    void UpdateExpandedState();
    bool IsPointInHeader(float x, float y) const;

  private:
    std::string m_Title;
    std::string m_IconClass;
    bool m_Expanded = true;
    bool m_Armed = false;

    // Internal structure
    UIElement* m_Header = nullptr;
    UIElement* m_Chevron = nullptr;
    UIElement* m_Icon = nullptr;
    Label* m_TitleLabel = nullptr;
    UIElement* m_ContentContainer = nullptr;

    ExpandedChangedHandler m_OnExpandedChanged;

    LazyContentBuilder m_LazyContentBuilder;
    bool m_LazyContentBuilt = false;
};

} // namespace GameEngine
