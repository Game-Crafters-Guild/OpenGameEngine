#pragma once

#include "Components/Terrain/TerrainSurfaceRules.h"
#include "Terrain/TerrainRuleConditionVocabulary.h"
#include "UI/UIElement.h"

namespace GameEngine::Editor
{

// The weight a condition gives every value across its authoring domain, drawn
// under that condition's range slider.
//
// It is not a decoration of the band, it is the band's MEANING: the plateau
// where the weight is 1, the feather ramps outside each edge, and — the part no
// pair of handles can show — the SHAPE of those ramps, which is the difference
// between the linear curve and the smoothstep
// an author picks for a softer corner.
//
// It plots TerrainRuleConditionWeightForValue, the same function the bake runs,
// rather than a drawing that resembles it. A picture of the maths that is not
// the maths is the kind of instrument that stays convincing after the maths
// changes underneath it.
//
// Deliberately inert: no pointer events, no drag state. The two handles above it
// own the interaction, and a second thing to grab on the same domain would be
// two answers to where the band starts.
class TerrainRuleWeightProfileStrip : public UIElement
{
  public:
    TerrainRuleWeightProfileStrip();

    // Re-reads on every inspector rebuild and on every drag frame, so it is a
    // plain copy rather than a subscription.
    void SetCondition(const Components::TerrainRuleCondition& condition,
                      TerrainRuleVocabulary::ConditionDomain domain);

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

  private:
    Components::TerrainRuleCondition m_Condition{};
    TerrainRuleVocabulary::ConditionDomain m_Domain{};
};

} // namespace GameEngine::Editor
