#include "Inspectors/ChromaticAberrationEffectInspector.h"
#include "InspectorRegistry.h"
#include "Components/Rendering/PostProcessEffects/ChromaticAberrationEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include <algorithm>

namespace GameEngine {
void RegisterChromaticAberrationEffectInspector()
{
    using namespace InspectorDrag;
    using CA = Components::ChromaticAberrationEffect;
    InspectorFn fn = [](const InspectorContext& ctx) {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid()) return;
        auto* c = ctx.World->GetComponent<CA>(ctx.Entity); if (!c) return;
        auto* w=ctx.World; auto e=ctx.Entity; auto* n=ctx.ChangeNotifications; auto* undo=ctx.Undo;
        AddComponentFloatRowWithDrag<CA>(ctx.Parent,"Lateral",c->Intensity,w,e,n,undo,"Change Chromatic Aberration Lateral",[](CA& x,float v){x.Intensity=std::clamp(v,0.0f,CA::kIntensityMax);},1.0f,"Lateral CA: maximum red/blue separation in pixels toward the frame edge",{},0.0f,CA::kIntensityMax);
        AddComponentFloatRowWithDrag<CA>(ctx.Parent,"Start Offset",c->StartOffset,w,e,n,undo,"Change Chromatic Aberration Start",[](CA& x,float v){x.StartOffset=std::clamp(v,0.0f,1.0f);},0.25f,"Normalized frame radius where lateral fringing and coma begin",{},0.0f,1.0f);
        AddComponentFloatRowWithDrag<CA>(ctx.Parent,"Saturation",c->Saturation,w,e,n,undo,"Change Chromatic Aberration Saturation",[](CA& x,float v){x.Saturation=std::clamp(v,0.0f,2.0f);},1.0f,"Color saturation of separated fringes",{},0.0f,2.0f);
        AddComponentFloatRowWithDrag<CA>(ctx.Parent,"Longitudinal",c->LongitudinalIntensity,w,e,n,undo,"Change Chromatic Aberration Longitudinal",[](CA& x,float v){x.LongitudinalIntensity=std::clamp(v,0.0f,CA::kLongitudinalMax);},0.0f,"Axial CA: maximum red/blue defocus radius in pixels, following camera focus and scene depth",{},0.0f,CA::kLongitudinalMax);
        AddComponentFloatRowWithDrag<CA>(ctx.Parent,"Coma",c->ComaIntensity,w,e,n,undo,"Change Chromatic Aberration Coma",[](CA& x,float v){x.ComaIntensity=std::clamp(v,0.0f,CA::kComaMax);},0.0f,"Radial comet-tail length in pixels for bright highlights at the frame corner",{},0.0f,CA::kComaMax);
    };
    InspectorRegistry::Get().RegisterComponentInspector<CA>(std::move(fn));
}
}
