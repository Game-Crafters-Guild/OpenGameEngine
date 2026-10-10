#include "Inspectors/DepthOfFieldEffectInspector.h"
#include "InspectorRegistry.h"
#include "Components/Rendering/PostProcessEffects/DepthOfFieldEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/EnumField.h"
#include <algorithm>

namespace GameEngine {
namespace {
constexpr EnumEntry<Components::DofSamplingQuality> kSamplingQualities[] = {
    {Components::DofSamplingQuality::Performance, "Performance"},
    {Components::DofSamplingQuality::Balanced, "Balanced"},
    {Components::DofSamplingQuality::Quality, "Quality"},
};
}

void RegisterDepthOfFieldEffectInspector()
{
    using namespace InspectorDrag;
    using DoF = Components::DepthOfFieldEffect;
    InspectorFn fn = [](const InspectorContext& ctx) {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid()) return;
        auto* c = ctx.World->GetComponent<DoF>(ctx.Entity); if (!c) return;
        auto* w=ctx.World; auto e=ctx.Entity; auto* n=ctx.ChangeNotifications; auto* undo=ctx.Undo;
        AddComponentFloatRowWithDrag<DoF>(ctx.Parent,"Max Radius",c->MaxRadius,w,e,n,undo,"Change Depth of Field Max Radius",[](DoF& x,float v){x.MaxRadius=std::clamp(v,0.0f,DoF::kMaxRadiusMax);},16.0f,"Artistic clamp on the blur radius in pixels. Focus, lens aperture, blade shape, anamorphic squeeze, and sensor size live on the Camera.",{},0.0f,DoF::kMaxRadiusMax);
        auto* samplingField = InspectorUI::AddEnumRow(ctx.Parent, "Sampling", kSamplingQualities, c->SamplingQuality,
            "Performance uses fewer radius samples, Balanced is the default, and Quality smooths very large bokeh at a higher GPU cost.");
        samplingField->SetOnValueChanged([w,e,n,undo](Components::DofSamplingQuality v) {
            CommitComponentWithUndo<DoF>(w,e,n,undo,"Change Depth of Field Sampling",
                [v](DoF& x){ x.SamplingQuality = v; });
        });
    };
    InspectorRegistry::Get().RegisterComponentInspector<DoF>(std::move(fn));
}
}
