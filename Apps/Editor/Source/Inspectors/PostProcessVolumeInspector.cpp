#include "Inspectors/PostProcessVolumeInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/VignetteEffect.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/TonemapMode.h"
#include "ECS/ComponentFactory.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "SceneView/PostProcessVolumeGizmo.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/PickerQueryFilter.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <any>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{

using Components::TonemapMode;
using Components::PostProcessVolumeShape;

static constexpr EnumEntry<TonemapMode> kTonemapModes[] = {
    {TonemapMode::ACES,     "ACES"},
    {TonemapMode::Reinhard, "Reinhard"},
    {TonemapMode::AgX,      "AgX"},
    {TonemapMode::Filmic,   "Filmic"},
    {TonemapMode::Neutral,  "Khronos PBR Neutral"},
    {TonemapMode::Linear,   "Linear"},
    {TonemapMode::GranTurismo7, "ICtCp Tonemapper (2025, GT7)"},
    {TonemapMode::ACES2,    "ACES 2"},
};

static constexpr EnumEntry<PostProcessVolumeShape> kVolumeShapes[] = {
    {PostProcessVolumeShape::Box,      "Box"},
    {PostProcessVolumeShape::Sphere,   "Sphere"},
    {PostProcessVolumeShape::Capsule,  "Capsule"},
    {PostProcessVolumeShape::Cylinder, "Cylinder"},
};

static constexpr EnumEntry<int32_t> kDitherModes[] = {
    {0, "Bayer 8x8"},
    {1, "Blue Noise"},
};

constexpr float kPostProcessEffectPickerWidthPx = 360.0f;

struct AddVolumeEffectChoice
{
    ECS::ComponentTypeId TypeId = 0;
};

void NotifyComponentChanged(Editor::EditorChangeNotifications* notifications,
                            ECS::World* world,
                            ECS::EntityHandle entity,
                            ECS::ComponentTypeId typeId)
{
    if (!notifications)
        return;
    notifications->NotifyComponentChanged(
        {world, entity, typeId, Editor::EditorChangeNotifications::ChangeKind::Commit});
}

// Adds one post-process effect through the Volume inspector's picker. Undo
// removes exactly what this command added, including a missing owner volume.
class AddVolumeEffectCommand final : public Editor::IEditorCommand
{
  public:
    AddVolumeEffectCommand(std::string name,
                           ECS::World* world,
                           Editor::EditorChangeNotifications* notifications,
                           ECS::EntityHandle entity,
                           ECS::ComponentTypeId effectTypeId)
        : m_Name(std::move(name)),
          m_World(world),
          m_Notifications(notifications),
          m_Entity(entity),
          m_EffectTypeId(effectTypeId)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Redo() override
    {
        m_AddedVolume = false;
        m_AddedEffect = false;
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        if (!m_World->HasComponent<Components::PostProcessVolume>(m_Entity))
        {
            m_World->AddComponentImmediate<Components::PostProcessVolume>(
                m_Entity, Components::PostProcessVolume{});
            m_AddedVolume = true;
            NotifyComponentChanged(
                m_Notifications, m_World, m_Entity,
                ECS::GetComponentTypeId<Components::PostProcessVolume>());
        }

        if (!m_World->HasComponent(m_Entity, m_EffectTypeId) &&
            ECS::ComponentFactory::Create(*m_World, m_Entity, m_EffectTypeId))
        {
            m_AddedEffect = true;
            ApplyAddDefaults();
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_EffectTypeId);
        }
    }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (m_AddedEffect && m_World->RemoveComponentByTypeIdImmediate(m_Entity, m_EffectTypeId))
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_EffectTypeId);
        if (m_AddedVolume)
        {
            m_World->RemoveComponentImmediate<Components::PostProcessVolume>(m_Entity);
            NotifyComponentChanged(
                m_Notifications, m_World, m_Entity,
                ECS::GetComponentTypeId<Components::PostProcessVolume>());
        }
    }

  private:
    void ApplyAddDefaults()
    {
        static const ECS::ComponentTypeId heightFogId =
            ECS::GetComponentTypeId<Components::HeightFogEffect>();
        if (m_EffectTypeId == heightFogId)
        {
            if (auto* fog = m_World->GetComponentForWrite<Components::HeightFogEffect>(m_Entity))
            {
                fog->Density = 0.65f;
                fog->Intensity = 1.0f;
                fog->SmoothLength = 1.0f;
            }
            return;
        }

        // Vignette's component default stays neutral so absent/partially
        // authored scenes remain byte-compatible, but an explicitly added
        // effect must be visible. Match the converter's authored default.
        static const ECS::ComponentTypeId vignetteId =
            ECS::GetComponentTypeId<Components::VignetteEffect>();
        if (m_EffectTypeId == vignetteId)
        {
            if (auto* vignette =
                    m_World->GetComponentForWrite<Components::VignetteEffect>(m_Entity))
                vignette->Intensity = 0.2f;
        }
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Entity{};
    ECS::ComponentTypeId m_EffectTypeId = 0;
    bool m_AddedVolume = false;
    bool m_AddedEffect = false;
};

// The registry effects the target volume does not already carry. The entity's
// effect set is snapshotted when the picker opens.
class PostProcessEffectSearchProvider final : public ISearchProvider
{
  public:
    PostProcessEffectSearchProvider(ECS::World* world, ECS::EntityHandle entity)
    {
        Rendering::PostProcessEffectRegistry::ForEach(
            [&](const Rendering::PostProcessEffectDescriptor& descriptor)
            {
                if (world && world->HasComponent(entity, descriptor.Type))
                    return;
                m_Entries.push_back({std::string(descriptor.DisplayName),
                                     std::string(descriptor.Description),
                                     std::string(descriptor.IconPath),
                                     descriptor.Type});
            });
        std::sort(m_Entries.begin(), m_Entries.end(),
                  [](const EffectEntry& a, const EffectEntry& b)
                  { return a.Label < b.Label; });
    }

    void BeginSearch(const std::string& query, ResultSink sink) override
    {
        const PickerQueryFilter filter = PickerQueryFilter::Parse(query);
        std::vector<SearchResultItem> results;
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            const auto& entry = m_Entries[i];
            if (!filter.Matches(entry.Label, entry.Description))
                continue;

            SearchResultItem item;
            item.Id = static_cast<SearchItemId>(i + 1);
            item.Label = entry.Label;
            item.Detail = entry.Description;
            item.Icon.CssClass = "search-result-icon-postfx";
            item.Icon.ImagePath = entry.IconPath;
            item.UserData = AddVolumeEffectChoice{entry.TypeId};
            results.push_back(std::move(item));
        }
        sink(std::move(results), true);
    }

    void CancelSearch() override {}
    std::string GetPlaceholderText() const override { return "Search effects..."; }

  private:
    struct EffectEntry
    {
        std::string Label;
        std::string Description;
        std::string IconPath;
        ECS::ComponentTypeId TypeId = 0;
    };

    std::vector<EffectEntry> m_Entries;
};

// Every registered post-process effect is an entry of a volume's stack:
// extraction reads effects only off volume entities.
bool IsPostProcessEffect(ECS::ComponentTypeId typeId)
{
    return Rendering::PostProcessEffectRegistry::Find(typeId) != nullptr;
}

void AddPostProcessEffectButton(const InspectorContext& ctx)
{
    if (ctx.Entities.size() > 1)
        return;

    ECS::World* world = ctx.World;
    const ECS::EntityHandle entity = ctx.Entity;
    Editor::EditorChangeNotifications* notifications = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    const std::function<ECS::World*()> getWorld = ctx.GetWorld;
    const std::function<void()> requestInspectorRefresh = ctx.RequestInspectorRefresh;

    auto button = std::make_unique<Button>();
    button->SetText("Add Post FX");
    button->SetTooltip("Add a post-process effect to this volume.");
    button->AddClass("inspector-add-component-button");
    button->Overrides()
        .Set(Style::Width, StyleLength::Px(kPostProcessEffectPickerWidthPx))
        .Set(Style::AlignSelf, AlignItems::Center)
        .Set(Style::MarginTop, StyleLength::Px(6.0f))
        .Set(Style::Order, 1);

    auto pickerOpen = std::make_shared<bool>(false);
    button->RegisterEventHandler(kEventButtonClick, [world, entity, notifications, undo, getWorld,
         requestInspectorRefresh, pickerOpen](UIEvent& e)
        {
            UIElement& source = *e.CurrentTarget;
            ECS::World* currentWorld = getWorld ? getWorld() : world;
            if (*pickerOpen || !currentWorld || !entity.IsValid() ||
                !currentWorld->IsValid(entity))
                return;

            UIManager* manager = source.GetOwnerManager();
            UIElement* root = manager ? manager->GetRootElement() : nullptr;
            if (!root)
                return;

            *pickerOpen = true;
            auto provider =
                std::make_shared<PostProcessEffectSearchProvider>(currentWorld, entity);
            auto dialog = std::make_unique<SearchDialog>();
            dialog->SetProvider(provider.get());
            dialog->SetFixedHeight(true);

            SearchDialog* dialogPtr = dialog.get();
            dialog->SetOnResult(
                [world, entity, notifications, undo, getWorld,
                 requestInspectorRefresh, pickerOpen, provider, dialogPtr, root](
                    const SearchResultItem& item)
                {
                    (void)provider; // Keeps the dialog's raw provider pointer valid.
                    const auto* choice = std::any_cast<AddVolumeEffectChoice>(&item.UserData);
                    if (!choice)
                        return;

                    const ECS::ComponentTypeId effectTypeId = choice->TypeId;
                    const std::string caption = "Add " + item.Label;
                    root->PostSafeAction(
                        [world, entity, notifications, undo, getWorld,
                         requestInspectorRefresh, pickerOpen, dialogPtr, root,
                         effectTypeId, caption]()
                        {
                            *pickerOpen = false;
                            root->RemoveChild(dialogPtr);
                            ECS::World* currentWorld = getWorld ? getWorld() : world;
                            if (!currentWorld || !entity.IsValid() ||
                                !currentWorld->IsValid(entity))
                                return;

                            if (undo)
                            {
                                undo->Execute(std::make_unique<AddVolumeEffectCommand>(
                                    caption, currentWorld, notifications, entity, effectTypeId));
                            }
                            else
                            {
                                AddVolumeEffectCommand command(
                                    caption, currentWorld, notifications, entity, effectTypeId);
                                command.Do();
                            }

                            if (requestInspectorRefresh)
                                requestInspectorRefresh();
                        });
                });
            dialog->SetOnCancel(
                [pickerOpen, provider, dialogPtr, root]()
                {
                    (void)provider; // Keeps the dialog's raw provider pointer valid.
                    *pickerOpen = false;
                    root->RemoveChild(dialogPtr);
                });

            const float buttonCenterX =
                source.GetLayoutX() + source.GetLayoutWidth() * 0.5f;
            const float anchorX =
                buttonCenterX - kPostProcessEffectPickerWidthPx * 0.5f;
            const float anchorY = source.GetLayoutY() + source.GetLayoutHeight();
            root->AddChild(std::move(dialog));
            dialogPtr->SetAnchorPosition(
                anchorX, anchorY, SearchDialogHorizontalAnchor::LeadingLeft,
                source.GetLayoutHeight());
            dialogPtr->Show();
        });

    ctx.Parent->AddChild(std::move(button));
}

} // namespace

void RegisterPostProcessVolumeInspector()
{
    using namespace InspectorDrag;
    using PP = Components::PostProcessVolume;

    Editor::SceneTools::RegisterPostProcessVolumeGizmo();

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* vol = ctx.World->GetComponent<PP>(ctx.Entity);
        if (!vol)
        {
            InspectorUI::AddLine(ctx.Parent, "(PostProcessVolume missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        AddToggleRow(ctx.Parent, "Global", vol->IsGlobal,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<PP>(w, e, n, undo, "Change PP Global",
                    [v](PP& u) { u.IsGlobal = v; });
            }, "When enabled, this volume affects the whole world. When disabled, the shape is sized and oriented by the entity's Transform.");

        // Priority & Weight
        AddComponentIntRowWithDrag<PP>(ctx.Parent, "Priority", vol->Priority, w, e, n,
            undo, "Change PP Priority",
            [](PP& u, int32_t v) { u.Priority = v; },
            0, "Blending priority (higher wins)");

        AddComponentFloatRowWithDrag<PP>(ctx.Parent, "Weight", vol->Weight, w, e, n,
            undo, "Change PP Weight",
            [](PP& u, float v) { u.Weight = std::clamp(v, 0.0f, 1.0f); },
            1.0f, "Blend weight [0..1]");

        AddComponentIntRowWithDrag<PP>(ctx.Parent, "PostProcessMask", static_cast<int>(vol->PostProcessMask), w, e, n,
            undo, "Change PP Layer Mask",
            [](PP& u, int v) { u.PostProcessMask = static_cast<uint32_t>(v); },
            static_cast<int>(0xFFFFFFFFu),
            "Camera filter bitmask: only cameras with (Camera.postProcessMask & this) != 0 receive this volume. -1 = all cameras.");

        // --- Bounds ---
        InspectorUI::AddTextBlock(ctx.Parent, "Bounds", "inspector-section-subheader");

        auto* shapeField = InspectorUI::AddEnumRow(ctx.Parent, "Shape", kVolumeShapes, vol->Shape,
            "Bounding shape: Box, Sphere, Capsule, or Cylinder. Transform controls orientation and scale.");
        shapeField->SetOnValueChanged([w, e, n, undo](PostProcessVolumeShape v) {
            CommitComponentWithUndo<PP>(w, e, n, undo, "Change PP Shape",
                [v](PP& u) { u.Shape = v; });
        });

        AddComponentFloatRowWithDrag<PP>(ctx.Parent, "BlendDistance", vol->BlendDistance, w, e, n,
            undo, "Change PP Blend Distance",
            [](PP& u, float v) { u.BlendDistance = std::max(0.0f, v); },
            1.0f, "World-space soft fade region around the shape");

        // Exposure is not a volume property — it lives on the Camera component (the sensor). See the
        // Camera inspector's Exposure section. Camera-less views (the editor Scene View) auto-meter
        // from the world default.

        // --- Tonemapping ---
        InspectorUI::AddTextBlock(ctx.Parent, "Tonemapping", "inspector-section-subheader");

        auto* tmField = InspectorUI::AddEnumRow(ctx.Parent, "Tonemap", kTonemapModes, vol->Tonemap,
            "Tonemap operator: ACES (legacy RRT+ODT fit), Reinhard, AgX, Filmic, Khronos PBR Neutral, Linear, ICtCp Tonemapper (2025, GT7), or ACES 2. "
            "In HDR, every operator except Linear extends its own output into the display's headroom above paper-white; Linear rolls the scene value directly.");
        tmField->SetOnValueChanged([w, e, n, undo](TonemapMode v) {
            CommitComponentWithUndo<PP>(w, e, n, undo, "Change Tonemap Mode",
                [v](PP& u) { u.Tonemap = v; });
        });

        auto* ditherField = InspectorUI::AddEnumRow(ctx.Parent, "DitherMode", kDitherModes, vol->DitherMode,
            "Dithering method for banding reduction");
        ditherField->SetOnValueChanged([w, e, n, undo](int32_t v) {
            CommitComponentWithUndo<PP>(w, e, n, undo, "Change Dither Mode",
                [v](PP& u) { u.DitherMode = v; });
        });

        AddComponentFloatRowWithDrag<PP>(ctx.Parent, "ICtCp Chroma Compression",
            std::clamp(vol->IctcpChromaCompression, 0.0f, 1.0f), w, e, n, undo,
            "Change ICtCp Chroma Compression",
            [](PP& u, float v) { u.IctcpChromaCompression = std::clamp(v, 0.0f, 1.0f); },
            0.0f,
            "Perceptually reduces chroma in highlights after tonemapping. Applies only to "
            "HDR10 PQ and HDR10+ output; SDR, HLG, and scRGB are unchanged. 0 = off.",
            {}, 0.0f, 1.0f);

        // The button is owned and mounted by the volume inspector. Nested effect
        // sections are appended later, and the button's flex order keeps it last.
        AddPostProcessEffectButton(ctx);
    };

    InspectorRegistry::Get().RegisterComponentInspector<PP>(std::move(fn));

    Editor::EditorComponentTraits traits;
    traits.HostsInspectorSection = IsPostProcessEffect;
    traits.HostedSectionBodyClass = "inspector-post-process-effect-body";
    traits.HostedEnableToggleTooltip = "Switch this effect on or off. Off pins it off wherever this volume "
                                       "applies, over any volume of lower priority.";
    traits.MissingHostBadge = "No Volume";
    traits.MissingHostWarning =
        "This effect has no Post Process Volume on its entity, so it is inactive. "
        "Add a Post Process Volume (the effect nests under it), or remove the effect.";
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<PP>(), std::move(traits));
}

} // namespace GameEngine
