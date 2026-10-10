#include "Inspectors/CubeLutEffectInspector.h"

#include "InspectorRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Core/Engine.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/AssetField.h"

#include <algorithm>

namespace GameEngine
{
namespace
{
using Components::CubeLutInputEncoding;
using Components::CubeLutTextureFormat;

static constexpr EnumEntry<CubeLutInputEncoding> kInputEncodings[] = {
    {CubeLutInputEncoding::Linear, "Linear"},
    {CubeLutInputEncoding::Rec709SRGB, "Rec.709 / sRGB"},
    {CubeLutInputEncoding::ArriLogC3, "ARRI LogC3"},
    {CubeLutInputEncoding::DaVinciWideGamutIntermediate, "DaVinci Intermediate"},
    {CubeLutInputEncoding::ACEScct, "ACEScct"},
    {CubeLutInputEncoding::Cineon, "Cineon"},
};

static constexpr EnumEntry<CubeLutTextureFormat> kTextureFormats[] = {
    {CubeLutTextureFormat::R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT"},
    {CubeLutTextureFormat::R16G16B16A16_FLOAT, "RGBA16F"},
};
} // namespace

void RegisterCubeLutEffectInspector()
{
    using namespace InspectorDrag;
    using LUT = Components::CubeLutEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<LUT>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* label = InspectorUI::AddLabel(row, "LUT", "Resolve .cube asset");
            if (label) label->AddClass("inspector-label-no-drag");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);

            auto field = std::make_unique<AssetField>();
            field->AddClass("dropdown-asset-field");
            field->SetAcceptedTypes({AssetType::CubeLut});
            if (auto* engine = &EngineCore::GetInstance(); engine->IsInitialized())
                field->SetAssetRegistry(&engine->GetAssetManager().GetRegistry());
            field->SetValue(effect->LutAssetGuid.ToGuid());
            field->SetOnValueChanged([w, e, n, undo](const GUID& guid) {
                CommitComponentWithUndo<LUT>(w, e, n, undo, "Change LUT Asset",
                    [guid](LUT& u) {
                        u.LutAssetGuid.Set(guid);
                    });
            });
            fieldContainer->AddChild(std::move(field));
        }

        AddComponentFloatRowWithDrag<LUT>(ctx.Parent, "Intensity",
            std::clamp(effect->Intensity, 0.0f, 1.0f), w, e, n, undo,
            "Change LUT Intensity",
            [](LUT& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); },
            1.0f,
            "0 = bypass, 1 = full LUT blend.",
            {},
            0.0f,
            1.0f);

        auto* encoding = InspectorUI::AddEnumRow(ctx.Parent, "Input Encoding", kInputEncodings,
            static_cast<CubeLutInputEncoding>(std::min<uint32>(effect->InputEncoding, 5u)),
            "Encoding expected by the LUT.");
        encoding->SetOnValueChanged([w, e, n, undo](CubeLutInputEncoding v) {
            CommitComponentWithUndo<LUT>(w, e, n, undo, "Change LUT Input Encoding",
                [v](LUT& u) { u.InputEncoding = static_cast<uint32>(v); });
        });

        auto* format = InspectorUI::AddEnumRow(ctx.Parent, "Texture Format", kTextureFormats,
            static_cast<CubeLutTextureFormat>(std::min<uint32>(effect->TextureFormat, 1u)),
            "GPU texture precision for uploaded LUT tables.");
        format->SetOnValueChanged([w, e, n, undo](CubeLutTextureFormat v) {
            CommitComponentWithUndo<LUT>(w, e, n, undo, "Change LUT Texture Format",
                [v](LUT& u) { u.TextureFormat = static_cast<uint32>(v); });
        });
    };

    InspectorRegistry::Get().RegisterComponentInspector<LUT>(std::move(fn));
}

} // namespace GameEngine
