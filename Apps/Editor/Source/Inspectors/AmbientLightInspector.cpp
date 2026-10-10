#include "Inspectors/AmbientLightInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/AmbientLight.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/EnumField.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <array>
#include <string>

namespace GameEngine
{
namespace
{

// Mode dropdown (mirrors Components::AmbientLightMode). Switching rebuilds the inspector so the
// Flat vs Gradient color controls show/hide (same pattern as SkyEnvironment's Sky Mode selector).
constexpr EnumEntry<Components::AmbientLightMode> kAmbientModes[] = {
    {Components::AmbientLightMode::Flat, "Flat"},
    {Components::AmbientLightMode::Gradient, "Gradient"},
};

// Allow modest over-1 authoring (HDR-ish fills), not just LDR. File-scope so the per-channel apply
// lambda can use it without capturing a local.
constexpr float kMaxAmbientColorChannel = 8.0f;

// Component color channels default to 1.0 (see AmbientLight.h) — the double-click reset target.
constexpr float kDefaultAmbientColorChannel = 1.0f;

// Well above any plausible authored fill (the shared reference white is 203 nits) while keeping
// the drag slider usable; matches kMaxAmbientColorChannel's role for the color rows.
constexpr float kMaxAmbientIntensityNits = 2000.0f;

// Per-channel linear-RGB rows for one float[3] color member. The engine authors ambient colors as
// linear (matching SkyEnvironment / the converter), so plain float fields edit them without a
// color-space round-trip. All rows go through the shared preview/commit/undo helpers.
using AmbientColorMember = float (Components::AmbientLight::*)[3];
void AddAmbientColorRows(const InspectorContext& ctx,
                         const char* section,
                         AmbientColorMember member,
                         const char* changePrefix)
{
    auto* al = ctx.World->GetComponent<Components::AmbientLight>(ctx.Entity);
    if (!al)
        return;

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    auto extras = InspectorDrag::GetAdditionalEntities(ctx);

    InspectorUI::AddTextBlock(ctx.Parent, section, "inspector-section-subheader");
    static constexpr std::array<const char*, 3> kChannels = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i)
    {
        InspectorDrag::AddComponentFloatRowWithDrag<Components::AmbientLight>(
            ctx.Parent, kChannels[i], (al->*member)[i], w, e, n, undo,
            std::string(changePrefix) + " " + kChannels[i],
            [member, i](Components::AmbientLight& u, float v) { (u.*member)[i] = std::clamp(v, 0.0f, kMaxAmbientColorChannel); },
            kDefaultAmbientColorChannel, "Linear RGB channel (0..1 typical).", extras, 0.0f, kMaxAmbientColorChannel);
    }
}

} // namespace

void RegisterAmbientLightInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* al = ctx.World->GetComponent<Components::AmbientLight>(ctx.Entity);
        if (!al)
        {
            InspectorUI::AddLine(ctx.Parent, "(AmbientLight missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        auto extras = InspectorDrag::GetAdditionalEntities(ctx);

        // Mode selector — rebuilds the inspector so Flat/Gradient color rows swap.
        {
            auto* modeField = InspectorUI::AddEnumRow(
                ctx.Parent, "Mode", kAmbientModes, al->Mode,
                "Flat: one constant ambient color. Gradient: sky/equator/ground ramp blended by "
                "surface normal (up=sky, horizon=equator, down=ground).");
            modeField->SetOnValueChanged([w, e, n, undo, extras](Components::AmbientLightMode v) {
                InspectorDrag::CommitComponentWithUndo<Components::AmbientLight>(
                    w, e, n, undo, "Change Ambient Mode",
                    [v](Components::AmbientLight& u) { u.Mode = v; });
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::AmbientLight>(ex);
                    if (!c)
                        continue;
                    Components::AmbientLight u = *c;
                    u.Mode = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
                if (n)
                {
                    n->NotifyComponentChange<Components::AmbientLight>(
                        w, e, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
                    for (auto& ex : extras)
                        n->NotifyComponentChange<Components::AmbientLight>(
                            w, ex, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
                }
            });
        }

        if (al->Mode == Components::AmbientLightMode::Flat)
        {
            AddAmbientColorRows(ctx, "Color (linear RGB)", &Components::AmbientLight::Color, "Change Ambient Color");
        }
        else
        {
            AddAmbientColorRows(ctx, "Sky color (linear RGB)", &Components::AmbientLight::SkyColor, "Change Ambient Sky");
            AddAmbientColorRows(ctx, "Equator color (linear RGB)", &Components::AmbientLight::EquatorColor, "Change Ambient Equator");
            AddAmbientColorRows(ctx, "Ground color (linear RGB)", &Components::AmbientLight::GroundColor, "Change Ambient Ground");
        }

        InspectorUI::AddTextBlock(ctx.Parent, "Lighting", "inspector-section-subheader");
        InspectorDrag::AddComponentFloatRowWithDrag<Components::AmbientLight>(
            ctx.Parent, "Intensity (nits)", al->Intensity, w, e, n, undo, "Change Ambient Intensity",
            [](Components::AmbientLight& u, float v) { u.Intensity = std::max(0.0f, v); },
            Components::kDefaultAmbientLightIntensityNits,
            "Floor luminance in nits on the 203-nit reference-white anchor. Color x this / 203 gives the "
            "scene-linear irradiance floor, on the same anchor as the sky and auto-exposure.",
            extras, 0.0f, kMaxAmbientIntensityNits);

        InspectorDrag::AddToggleRow(
            ctx.Parent, "Affect specular", al->AffectSpecular,
            [w, e, n, undo, extras](bool v) {
                InspectorDrag::CommitComponentWithUndo<Components::AmbientLight>(
                    w, e, n, undo, "Change Ambient Affect Specular",
                    [v](Components::AmbientLight& u) { u.AffectSpecular = v; });
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::AmbientLight>(ex);
                    if (!c)
                        continue;
                    Components::AmbientLight u = *c;
                    u.AffectSpecular = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            },
            "Off (default): the floor lifts diffuse only; specular reflections keep coming from the "
            "sky/probes. On: the floor also fills the primary reflection lobe.");
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::AmbientLight>(std::move(fn));
}

} // namespace GameEngine
