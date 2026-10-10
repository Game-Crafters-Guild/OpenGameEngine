#include "Graph/GraphOverlay.h"

#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIPrimitive.h"
#include "UI/UIStyle.h"

#include <memory>

namespace GameEngine {

GraphOverlay::GraphOverlay()
{
    AddClass("graph-overlay");
    SetFocusable(false);
    Overrides()
        .Set(Style::PointerEvents, false)
        .Set(Style::ZIndex, 1)
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f));
}

void GraphOverlay::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& /*style*/,
                                        float /*x*/, float /*y*/, float /*w*/, float /*h*/)
{
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }
}

} // namespace GameEngine

namespace RegisterGraphElements
{
static auto s_reg_graphOverlay =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::GraphOverlay>(
        "GraphOverlay",
        []() { return std::make_unique<GameEngine::GraphOverlay>(); })
        .TagAlias("graphoverlay");
}
