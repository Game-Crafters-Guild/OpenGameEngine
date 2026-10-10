#pragma once

#include <string>

namespace GameEngine
{
class Label;
class UIElement;
struct InspectorContext;

// The lines the sky's sun readout card shows, top to bottom; an empty line is not shown.
struct SkySunReadoutLines
{
    std::string Day;
    std::string Noon;
    std::string Now;
    std::string Moon;
};

// The sky inspector's sun readout: one always-visible info card (UI/InfoCard.h, styled by the
// card's own sheet) holding a line per figure. Always visible because it is live data about the
// scene, not explanatory copy the "Show Info Cards" setting hides.
class SkySunReadoutCard
{
public:
    // Builds the card as the last child of `parent`.
    explicit SkySunReadoutCard(UIElement* parent);

    // Shows `lines`, rewriting only the lines whose text changed.
    void Show(const SkySunReadoutLines& lines);

    UIElement* GetCard() const { return m_Card; }

private:
    UIElement* m_Card = nullptr;
    Label* m_Day = nullptr;
    Label* m_Noon = nullptr;
    Label* m_Now = nullptr;
    Label* m_Moon = nullptr;
};

// Adds the card to the sky inspector, filled from the sky's path and the light it drives, and keeps
// it current while the scene runs: sunrise, sunset and noon for the sky's day, then what the driven
// light delivers now, split between the sun and the moon, or why the sky lights nothing.
void AddSkySunReadoutCard(UIElement* parent, const InspectorContext& ctx);

} // namespace GameEngine
