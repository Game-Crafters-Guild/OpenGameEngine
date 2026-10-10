#pragma once

#include "UI/UIElement.h"

namespace GameEngine {

/** In-flow overlay sibling of the node layer. Empty until marquee (B2/B3). */
class GraphOverlay : public UIElement {
public:
    GraphOverlay();
    ~GraphOverlay() override = default;

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;
};

} // namespace GameEngine
