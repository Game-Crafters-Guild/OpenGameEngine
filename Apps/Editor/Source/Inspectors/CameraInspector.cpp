#include "Inspectors/CameraInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/Camera.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Panels/CameraCustomAspectModal.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "UI/UIManager.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
using Engine::Renderer::CameraAspectPreset;
using Engine::Renderer::CameraAspectPresetFromDropdownValue;
using Engine::Renderer::GetCameraAspectPresetDropdownValue;

static constexpr EnumEntry<Components::CameraSensorPreset> kSensorPresets[] = {
    {Components::CameraSensorPreset::Custom,           "Custom"},
    {Components::CameraSensorPreset::Standard8,        "Standard 8 mm"},
    {Components::CameraSensorPreset::Super8,           "Super 8"},
    {Components::CameraSensorPreset::Film16,           "16 mm"},
    {Components::CameraSensorPreset::Super16,          "Super 16"},
    {Components::CameraSensorPreset::Film35TwoPerf,    "35 mm 2-perf"},
    {Components::CameraSensorPreset::Film35ThreePerf,  "35 mm 3-perf"},
    {Components::CameraSensorPreset::Film35Academy,    "35 mm Academy (4-perf)"},
    {Components::CameraSensorPreset::Super35,          "Super 35"},
    {Components::CameraSensorPreset::Film65FivePerf,   "65 mm (5-perf)"},
    {Components::CameraSensorPreset::Imax15Perf,       "IMAX 15/70"},
    {Components::CameraSensorPreset::MicroFourThirds,  "Micro Four Thirds"},
    {Components::CameraSensorPreset::ApsC,             "APS-C"},
    {Components::CameraSensorPreset::FullFrame,        "Full Frame 35 mm"},
    {Components::CameraSensorPreset::MediumFormat4433, "Medium Format 44x33 (Fujifilm GFX)"},
    {Components::CameraSensorPreset::MediumFormat5440, "Medium Format 54x40 (Phase One)"},
    {Components::CameraSensorPreset::ArriAlexa35,      "ARRI Alexa 35"},
    {Components::CameraSensorPreset::ArriAlexaLF,      "ARRI Alexa LF"},
    {Components::CameraSensorPreset::ArriAlexa65,      "ARRI Alexa 65"},
};

static constexpr EnumEntry<Components::ExposureMode> kExposureModes[] = {
    {Components::ExposureMode::Fixed,    "Fixed (linear)",    "dropdown-icon--exposure-fixed"},
    {Components::ExposureMode::Manual,   "Manual (EV100)",    "dropdown-icon--exposure-manual"},
    {Components::ExposureMode::Physical, "Physical (camera)", "dropdown-icon--exposure-physical"},
    {Components::ExposureMode::Auto,     "Auto (metered)",    "dropdown-icon--exposure-auto"},
};

CameraCustomAspectModal* g_CustomAspectModal = nullptr;
bool g_CustomAspectModalMounted = false;

void EnsureCustomAspectModal(UIElement* anchor)
{
    if (g_CustomAspectModalMounted || !anchor)
        return;

    UIManager* mgr = anchor->GetOwnerManager();
    if (!mgr)
        return;

    UIElement* root = mgr->GetRootElement();
    if (!root)
        return;

    auto modal = std::make_unique<CameraCustomAspectModal>();
    g_CustomAspectModal = modal.get();
    root->AddChild(std::move(modal));
    g_CustomAspectModalMounted = true;
}

void CommitCameraAspectPreset(ECS::World* w,
                              ECS::EntityHandle e,
                              Editor::EditorChangeNotifications* n,
                              Editor::UndoRedoService* undo,
                              const std::vector<ECS::EntityHandle>& extras,
                              CameraAspectPreset preset,
                              float customWidth,
                              float customHeight)
{
    const auto apply = [&](Components::Camera& camera)
    {
        camera.AspectPreset = static_cast<uint32>(preset);
        if (preset == CameraAspectPreset::Custom)
        {
            camera.CustomAspectWidth = std::max(customWidth, 0.0001f);
            camera.CustomAspectHeight = std::max(customHeight, 0.0001f);
        }
    };

    if (undo)
    {
        auto target = extras.empty()
            ? InspectorDrag::MakeComponentSnapshotTarget<Components::Camera>(w, e, n, "Change Camera Aspect Ratio")
            : InspectorDrag::MakeMultiComponentSnapshotTarget<Components::Camera>(w, e, extras, n, "Change Camera Aspect Ratio");
        auto edit = undo->BeginInteractiveEdit("Change Camera Aspect Ratio", std::move(target));
        if (auto* comp = w->GetComponent<Components::Camera>(e))
        {
            Components::Camera updated = *comp;
            apply(updated);
            w->AddComponentImmediate(e, updated);
        }
        for (auto& ex : extras)
        {
            if (auto* c = w->GetComponent<Components::Camera>(ex))
            {
                Components::Camera updated = *c;
                apply(updated);
                w->AddComponentImmediate(ex, updated);
            }
        }
        edit.Commit();
        return;
    }

    if (auto* comp = w->GetComponent<Components::Camera>(e))
    {
        Components::Camera updated = *comp;
        apply(updated);
        Editor::CommitComponentUpdate(w, e, n, updated);
    }
    for (auto& ex : extras)
    {
        if (auto* c = w->GetComponent<Components::Camera>(ex))
        {
            Components::Camera updated = *c;
            apply(updated);
            Editor::CommitComponentUpdate(w, ex, n, updated);
        }
    }
}

} // namespace

void RegisterCameraInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        {
            return;
        }

        auto* cam = ctx.World->GetComponent<Components::Camera>(ctx.Entity);
        if (!cam)
        {
            InspectorUI::AddLine(ctx.Parent, "(Camera missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        using namespace InspectorDrag;
        auto extras = GetAdditionalEntities(ctx);

        // Toggling Perspective swaps the visible projection parameter row
        // (FovY <-> OrthographicSize), so fire an InspectorRebuild after the
        // commit to refresh the inspector against the new mode.
        auto notifyInspectorRebuild = [w, e, n]()
        {
            if (!n) return;
            Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
            ev.world = w;
            ev.entity = e;
            ev.componentType = ECS::GetComponentTypeId<Components::Camera>();
            ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
            n->NotifyComponentChanged(ev);
        };

        AddToggleRow(ctx.Parent, "Perspective", cam->Perspective,
            [w, e, n, undo, extras, notifyInspectorRebuild](bool v) {
                if (undo)
                {
                    auto target = extras.empty()
                        ? MakeComponentSnapshotTarget<Components::Camera>(w, e, n, "Change Camera Perspective")
                        : MakeMultiComponentSnapshotTarget<Components::Camera>(w, e, extras, n, "Change Camera Perspective");
                    auto edit = undo->BeginInteractiveEdit("Change Camera Perspective", std::move(target));
                    auto* comp = w->GetComponent<Components::Camera>(e);
                    if (!comp) return;
                    Components::Camera updated = *comp;
                    updated.Perspective = v;
                    w->AddComponentImmediate(e, updated);
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::Camera>(ex);
                        if (!c) continue;
                        Components::Camera u = *c;
                        u.Perspective = v;
                        w->AddComponentImmediate(ex, u);
                    }
                    edit.Commit();
                    notifyInspectorRebuild();
                    return;
                }

                auto* comp = w->GetComponent<Components::Camera>(e);
                if (!comp) return;
                Components::Camera updated = *comp;
                updated.Perspective = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::Camera>(ex);
                    if (!c) continue;
                    Components::Camera u = *c;
                    u.Perspective = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
                notifyInspectorRebuild();
            }, "Enable perspective projection; disable for orthographic");

        // Only show the projection parameter relevant to the current mode, to
        // avoid implying the inactive one has any effect.
        auto focalLengthFieldSlot = std::make_shared<FloatField*>(nullptr);
        if (cam->Perspective)
        {
            auto fovFieldSlot = std::make_shared<FloatField*>(nullptr);
            auto fovHandlers = MakeComponentInteractiveHandlers<Components::Camera, float>(
                w, e, n, undo, "Change Camera FOV Y",
                [](Components::Camera& u, float v) { u.FovY = std::clamp(v, 0.1f, 179.0f); },
                extras);
            const auto syncFocalLengthField = [w, e, focalLengthFieldSlot]() {
                const auto* camera = w->GetComponent<Components::Camera>(e);
                FloatField* field = *focalLengthFieldSlot;
                if (camera && field)
                    field->SetValueWithoutNotify(
                        Components::CameraFocalLengthMmFromVerticalFov(
                            camera->FovY, camera->SensorHeightMm));
            };
            *fovFieldSlot = AddFloatRowWithDrag(
                ctx.Parent, "FovY", cam->FovY,
                [preview = std::move(fovHandlers.first), syncFocalLengthField](float v) {
                    preview(v);
                    syncFocalLengthField();
                },
                [commit = std::move(fovHandlers.second), syncFocalLengthField](float v) {
                    commit(v);
                    syncFocalLengthField();
                },
                cam->FovY, "Vertical field of view in degrees", 0.1f, 179.0f);

            auto focalLengthHandlers = MakeComponentInteractiveHandlers<Components::Camera, float>(
                w, e, n, undo, "Change Camera Focal Length",
                [](Components::Camera& u, float v) {
                    u.FovY = Components::CameraVerticalFovFromFocalLengthMm(v, u.SensorHeightMm);
                },
                extras);
            const auto syncFovField = [w, e, fovFieldSlot]() {
                const auto* camera = w->GetComponent<Components::Camera>(e);
                FloatField* field = *fovFieldSlot;
                if (camera && field)
                    field->SetValueWithoutNotify(camera->FovY);
            };
            *focalLengthFieldSlot = AddFloatRowWithDrag(
                ctx.Parent, "Focal Length (mm)",
                Components::CameraFocalLengthMmFromVerticalFov(cam->FovY, cam->SensorHeightMm),
                [preview = std::move(focalLengthHandlers.first), syncFovField](float v) {
                    preview(v);
                    syncFovField();
                },
                [commit = std::move(focalLengthHandlers.second), syncFovField](float v) {
                    commit(v);
                    syncFovField();
                },
                50.0f,
                "Physical focal length for the selected sensor format. Editing this updates FoV Y; 300 mm on full frame gives about 4.6 degrees vertically.",
                1.0f, 2000.0f);
        }
        else
        {
            // Commit a single bool field with undo, mirroring the Perspective toggle.
            const auto commitBool = [w, e, n, undo, extras](const char* undoLabel,
                                                            bool value,
                                                            void (*setter)(Components::Camera&, bool))
            {
                if (undo)
                {
                    auto target = extras.empty()
                        ? MakeComponentSnapshotTarget<Components::Camera>(w, e, n, undoLabel)
                        : MakeMultiComponentSnapshotTarget<Components::Camera>(w, e, extras, n, undoLabel);
                    auto edit = undo->BeginInteractiveEdit(undoLabel, std::move(target));
                    if (auto* comp = w->GetComponent<Components::Camera>(e))
                    {
                        Components::Camera updated = *comp;
                        setter(updated, value);
                        w->AddComponentImmediate(e, updated);
                    }
                    for (auto& ex : extras)
                    {
                        if (auto* c = w->GetComponent<Components::Camera>(ex))
                        {
                            Components::Camera u = *c;
                            setter(u, value);
                            w->AddComponentImmediate(ex, u);
                        }
                    }
                    edit.Commit();
                    return;
                }

                if (auto* comp = w->GetComponent<Components::Camera>(e))
                {
                    Components::Camera updated = *comp;
                    setter(updated, value);
                    Editor::CommitComponentUpdate(w, e, n, updated);
                }
                for (auto& ex : extras)
                {
                    if (auto* c = w->GetComponent<Components::Camera>(ex))
                    {
                        Components::Camera u = *c;
                        setter(u, value);
                        Editor::CommitComponentUpdate(w, ex, n, u);
                    }
                }
            };

            // Pixel-perfect toggle sits above the orthographic-size row: enabling it
            // computes the ortho size from the reference resolution and hides the
            // authored OrthographicSize (which is then ignored).
            AddToggleRow(ctx.Parent, "Pixel Perfect", cam->PixelPerfect,
                [commitBool, notifyInspectorRebuild](bool v) {
                    commitBool("Change Camera Pixel Perfect", v,
                        [](Components::Camera& u, bool value) { u.PixelPerfect = value; });
                    notifyInspectorRebuild();
                },
                "Lock orthographic size to an integer upscale of the reference resolution for crisp pixel art");

            if (cam->PixelPerfect)
            {
                AddComponentIntRowWithDrag<Components::Camera>(ctx.Parent, "Pixels Per Unit", static_cast<int>(cam->PixelPerfectPixelsPerUnit), w, e, n, undo,
                    "Change Camera Pixels Per Unit",
                    [](Components::Camera& u, int v) { u.PixelPerfectPixelsPerUnit = static_cast<uint32>(std::max(1, v)); },
                    static_cast<int>(cam->PixelPerfectPixelsPerUnit), "Reference pixel size: source texels per world unit", extras);
                AddComponentIntRowWithDrag<Components::Camera>(ctx.Parent, "Reference Width", static_cast<int>(cam->PixelPerfectReferenceWidth), w, e, n, undo,
                    "Change Camera Reference Width",
                    [](Components::Camera& u, int v) { u.PixelPerfectReferenceWidth = static_cast<uint32>(std::max(1, v)); },
                    static_cast<int>(cam->PixelPerfectReferenceWidth), "Reference render width the integer upscale is fit against", extras);
                AddComponentIntRowWithDrag<Components::Camera>(ctx.Parent, "Reference Height", static_cast<int>(cam->PixelPerfectReferenceHeight), w, e, n, undo,
                    "Change Camera Reference Height",
                    [](Components::Camera& u, int v) { u.PixelPerfectReferenceHeight = static_cast<uint32>(std::max(1, v)); },
                    static_cast<int>(cam->PixelPerfectReferenceHeight), "Reference render height the integer upscale is fit against", extras);
                AddToggleRow(ctx.Parent, "Pixel Snap", cam->PixelPerfectPixelSnap,
                    [commitBool](bool v) {
                        commitBool("Change Camera Pixel Snap", v,
                            [](Components::Camera& u, bool value) { u.PixelPerfectPixelSnap = value; });
                    },
                    "Snap the camera's world X/Y to the pixel grid to avoid shimmer when moving");
                AddToggleRow(ctx.Parent, "Expand", cam->PixelPerfectExpand,
                    [commitBool](bool v) {
                        commitBool("Change Camera Pixel Perfect Expand", v,
                            [](Components::Camera& u, bool value) { u.PixelPerfectExpand = value; });
                    },
                    "Off: letterbox to the reference resolution. On: reveal more world to fill non-matching screen aspects. Expand is suppressed when an aspect preset is set");
            }
            else
            {
                AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "OrthographicSize", cam->OrthographicSize, w, e, n, undo,
                    "Change Camera Orthographic Size",
                    [](Components::Camera& u, float v) { u.OrthographicSize = std::max(0.0001f, v); },
                    cam->OrthographicSize, "Vertical world-space extent of the orthographic view volume", extras);
            }
        }
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "NearZ", cam->NearZ, w, e, n, undo,
            "Change Camera Near Z",
            [](Components::Camera& u, float v) { u.NearZ = std::max(0.0001f, v); },
            cam->NearZ, "Near clipping plane distance", extras);
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "FarZ", cam->FarZ, w, e, n, undo,
            "Change Camera Far Z",
            [](Components::Camera& u, float v) { u.FarZ = std::max(1.0f, v); },
            cam->FarZ, "Far clipping plane distance", extras);

        {
            static const std::vector<Dropdown::Option> kAspectOptions = {
                {"native", "Native (No Bars)"},
                {"16:9", "16:9"},
                {"4:3", "4:3"},
                {"16:10", "16:10"},
                {"21:9", "21:9"},
                {"custom", "Custom..."},
            };

            const std::string selectedAspect = GetCameraAspectPresetDropdownValue(*cam);
            int selectedIndex = 0;
            for (size_t i = 0; i < kAspectOptions.size(); ++i)
            {
                if (kAspectOptions[i].value == selectedAspect)
                {
                    selectedIndex = static_cast<int>(i);
                    break;
                }
            }

            EnsureCustomAspectModal(ctx.Parent);
            Dropdown* aspectDropdown = InspectorUI::AddDropdownRow(
                ctx.Parent,
                "Aspect Ratio",
                kAspectOptions,
                selectedIndex,
                "Target projection aspect ratio. Non-native presets add black bars in Game View, Player, and Scene View (2D/orthographic).");
            aspectDropdown->SetOnValueChanged([w, e, n, undo, extras, aspectDropdown](const std::string& value)
            {
                auto* comp = w->GetComponent<Components::Camera>(e);
                if (!comp)
                    return;

                const std::string previousValue = GetCameraAspectPresetDropdownValue(*comp);
                if (value == "custom")
                {
                    if (!g_CustomAspectModal)
                        return;

                    g_CustomAspectModal->SetOnCommit([w, e, n, undo, extras, aspectDropdown](float width, float height)
                    {
                        CommitCameraAspectPreset(
                            w,
                            e,
                            n,
                            undo,
                            extras,
                            CameraAspectPreset::Custom,
                            width,
                            height);
                        aspectDropdown->SetSelectedValue("custom");
                    });
                    g_CustomAspectModal->SetOnCancel([aspectDropdown, previousValue]()
                    {
                        aspectDropdown->SetSelectedValue(previousValue);
                    });
                    g_CustomAspectModal->Show(comp->CustomAspectWidth, comp->CustomAspectHeight);
                    return;
                }

                CommitCameraAspectPreset(
                    w,
                    e,
                    n,
                    undo,
                    extras,
                    CameraAspectPresetFromDropdownValue(value),
                    comp->CustomAspectWidth,
                    comp->CustomAspectHeight);
            });
        }

        // Editable: CullingMask, PostProcessProfileId
        AddComponentIntRowWithDrag<Components::Camera>(ctx.Parent, "CullingMask", static_cast<int>(cam->CullingMask), w, e, n, undo,
            "Change Camera Culling Mask",
            [](Components::Camera& u, int v) { u.CullingMask = static_cast<uint32>(std::max(0, v)); },
            static_cast<int>(cam->CullingMask), "Layer visibility mask for this camera", extras);
        AddComponentIntRowWithDrag<Components::Camera>(ctx.Parent, "PostProcessProfileId", static_cast<int>(cam->PostProcessProfileId), w, e, n, undo,
            "Change Camera PostProcess Profile",
            [](Components::Camera& u, int v) { u.PostProcessProfileId = static_cast<uint32>(std::max(0, v)); },
            static_cast<int>(cam->PostProcessProfileId), "Post-processing profile asset ID", extras);
        {
            static const std::vector<Dropdown::Option> kAAModeOptions = {
                {"0", "Default"},
                {"1", "Off"},
                {"2", "MSAA"},
                {"3", "TAA"},
                {"4", "FXAA"},
                {"5", "SMAA"},
                {"6", "FXAA (Two-Frame)"},
            };
            const auto selectedMode = std::to_string(cam->AntiAliasing);
            int selectedModeIndex = 0;
            for (size_t i = 0; i < kAAModeOptions.size(); ++i)
            {
                if (kAAModeOptions[i].value == selectedMode)
                {
                    selectedModeIndex = static_cast<int>(i);
                    break;
                }
            }

            Dropdown* aaModeDropdown = InspectorUI::AddDropdownRow(
                ctx.Parent,
                "Anti-Aliasing",
                kAAModeOptions,
                selectedModeIndex,
                "Per-camera AA mode. Default inherits the engine setting; MSAA uses the sample "
                "count below; TAA renders single-sample with temporal accumulation; FXAA is the "
                "classic single-frame filter; FXAA (Two-Frame) runs it under an edge jitter and "
                "blends two frames (no accumulated history, no ghosting trail); SMAA "
                "reconstructs edge shapes in post without jitter. Supersampling (SSAA) is the "
                "Render Scale below, above 1.0 — it composes with every mode except MSAA.");
            aaModeDropdown->SetOnValueChanged([w, e, n, undo, extras](const std::string& value)
            {
                uint32_t mode = 0u;
                try
                {
                    mode = static_cast<uint32_t>(std::stoul(value));
                }
                catch (...)
                {
                    mode = 0u;
                }
                if (mode > 5u)
                    mode = 0u;

                const auto applyMode = [mode](Components::Camera& u) { u.AntiAliasing = mode; };
                if (undo)
                {
                    auto target = extras.empty()
                        ? MakeComponentSnapshotTarget<Components::Camera>(w, e, n, "Change Camera Anti-Aliasing")
                        : MakeMultiComponentSnapshotTarget<Components::Camera>(w, e, extras, n, "Change Camera Anti-Aliasing");
                    auto edit = undo->BeginInteractiveEdit("Change Camera Anti-Aliasing", std::move(target));
                    auto* comp = w->GetComponent<Components::Camera>(e);
                    if (!comp) return;
                    Components::Camera updated = *comp;
                    applyMode(updated);
                    w->AddComponentImmediate(e, updated);
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::Camera>(ex);
                        if (!c) continue;
                        Components::Camera u = *c;
                        applyMode(u);
                        w->AddComponentImmediate(ex, u);
                    }
                    edit.Commit();
                    return;
                }

                auto* comp = w->GetComponent<Components::Camera>(e);
                if (!comp) return;
                Components::Camera updated = *comp;
                applyMode(updated);
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::Camera>(ex);
                    if (!c) continue;
                    Components::Camera u = *c;
                    applyMode(u);
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            });
        }
        {
            static const std::vector<Dropdown::Option> kMsaaOptions = {
                {"0", "Default"},
                {"1", "Off"},
                {"2", "2x"},
                {"4", "4x"},
                {"8", "8x"},
            };
            const auto selectedMsaa = std::to_string(cam->MSAASamples);
            int selectedIndex = 0;
            for (size_t i = 0; i < kMsaaOptions.size(); ++i)
            {
                if (kMsaaOptions[i].value == selectedMsaa)
                {
                    selectedIndex = static_cast<int>(i);
                    break;
                }
            }

            Dropdown* msaaDropdown = InspectorUI::AddDropdownRow(
                ctx.Parent,
                "MSAA",
                kMsaaOptions,
                selectedIndex,
                "Per-camera Game View MSAA. Default uses the same sample count as the scene view.");
            msaaDropdown->SetOnValueChanged([w, e, n, undo, extras](const std::string& value)
            {
                uint32_t sampleCount = 0u;
                try
                {
                    sampleCount = static_cast<uint32_t>(std::stoul(value));
                }
                catch (...)
                {
                    sampleCount = 0u;
                }
                if (sampleCount != 0u && sampleCount != 1u && sampleCount != 2u && sampleCount != 4u && sampleCount != 8u)
                    sampleCount = 0u;

                if (undo)
                {
                    auto target = extras.empty()
                        ? MakeComponentSnapshotTarget<Components::Camera>(w, e, n, "Change Camera MSAA")
                        : MakeMultiComponentSnapshotTarget<Components::Camera>(w, e, extras, n, "Change Camera MSAA");
                    auto edit = undo->BeginInteractiveEdit("Change Camera MSAA", std::move(target));
                    auto* comp = w->GetComponent<Components::Camera>(e);
                    if (!comp) return;
                    Components::Camera updated = *comp;
                    updated.MSAASamples = sampleCount;
                    w->AddComponentImmediate(e, updated);
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::Camera>(ex);
                        if (!c) continue;
                        Components::Camera u = *c;
                        u.MSAASamples = sampleCount;
                        w->AddComponentImmediate(ex, u);
                    }
                    edit.Commit();
                    return;
                }

                auto* comp = w->GetComponent<Components::Camera>(e);
                if (!comp) return;
                Components::Camera updated = *comp;
                updated.MSAASamples = sampleCount;
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::Camera>(ex);
                    if (!c) continue;
                    Components::Camera u = *c;
                    u.MSAASamples = sampleCount;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            });
        }
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Render Scale", cam->RenderScale, w, e, n, undo,
            "Change Camera Render Scale",
            [](Components::Camera& u, float v) {
                // 0 = inherit the engine default (Dynamic included); an
                // explicit value clamps to the engine's supported range.
                u.RenderScale = v <= 0.0f ? 0.0f : std::clamp(v, 0.25f, 2.0f);
            },
            cam->RenderScale,
            "Per-camera render scale override. 0 = inherit the engine default (including Dynamic "
            "resolution). An explicit 0.25-2.0 pins this camera to a fixed scale; above 1.0 "
            "supersamples (SSAA). Not with MSAA — a multisampled view cannot be scaled.", extras);
        AddComponentIntRowWithDrag<Components::Camera>(ctx.Parent, "PostProcessMask", static_cast<int>(cam->PostProcessMask), w, e, n, undo,
            "Change Camera PostProcess Mask",
            [](Components::Camera& u, int v) { u.PostProcessMask = static_cast<uint32>(v); },
            static_cast<int>(cam->PostProcessMask),
            "Volume layer bitmask: only volumes with (PostProcessVolume.postProcessMask & this) != 0 affect this camera. -1 = accept all.", extras);

        // --- Exposure (the camera's sensor; the active camera's exposure drives its view) ---
        InspectorUI::AddTextBlock(ctx.Parent, "Exposure", "inspector-section-subheader");

        auto* exModeField = InspectorUI::AddEnumRow(ctx.Parent, "Exposure Mode", kExposureModes, cam->ExposureControl,
            "Fixed = a linear multiplier. Manual = an absolute photographic EV100 anchored to 203-nit "
            "reference white (sun ~15, overcast ~12, interior ~7-9, night ~2-5). Physical = EV100 from "
            "camera Aperture/Shutter/ISO. Auto = histogram metering + eye adaptation.");
        exModeField->SetOnValueChanged([w, e, n, undo, extras](Components::ExposureMode v) {
            CommitComponentWithUndo<Components::Camera>(w, e, n, undo, "Change Exposure Mode",
                [v](Components::Camera& u) { u.ExposureControl = v; });
            // Switching mode shows/hides the mode-specific rows below, so rebuild the inspector layout.
            if (n)
            {
                n->NotifyComponentChange<Components::Camera>(w, e, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
                for (auto& ex : extras)
                    n->NotifyComponentChange<Components::Camera>(w, ex, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
            }
        });

        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Exposure Compensation", cam->ExposureCompensation, w, e, n,
            undo, "Change Exposure Compensation",
            [](Components::Camera& u, float v) { u.ExposureCompensation = v; },
            0.0f, "+/- stops applied in every mode (+ brightens).", extras);

        if (cam->ExposureControl == Components::ExposureMode::Fixed)
        {
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Exposure", cam->Exposure, w, e, n,
                undo, "Change Exposure",
                [](Components::Camera& u, float v) { u.Exposure = std::max(0.01f, v); },
                1.0f, "Fixed mode: linear multiplier applied before tonemapping (1 = identity).", extras);
        }
        else if (cam->ExposureControl == Components::ExposureMode::Manual)
        {
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Manual EV100", cam->ManualExposureEV, w, e, n,
                undo, "Change Manual EV",
                [](Components::Camera& u, float v) { u.ManualExposureEV = v; },
                Components::kDefaultManualExposureEv,
                "Manual mode: absolute photographic EV100. Sunny ~15, overcast ~12, interior ~7-9, night ~2-5.", extras);
        }
        else if (cam->ExposureControl == Components::ExposureMode::Physical)
        {
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Shutter (s)", cam->ShutterTime, w, e, n,
                undo, "Change Shutter Time",
                [](Components::Camera& u, float v) { u.ShutterTime = std::max(1.0e-5f, v); },
                0.01f, "Physical mode: shutter time in seconds. Longer = brighter. Sunny-16 uses 1/100 s = 0.01.", extras);
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "ISO", cam->Iso, w, e, n,
                undo, "Change ISO",
                [](Components::Camera& u, float v) { u.Iso = std::max(1.0f, v); },
                100.0f, "Physical mode: sensor sensitivity. Higher = brighter. Sunny-16 uses ISO 100.", extras);
        }
        else if (cam->ExposureControl == Components::ExposureMode::Auto)
        {
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Auto Min EV", cam->AutoExposureMinEv, w, e, n,
                undo, "Change Auto Min EV",
                [](Components::Camera& u, float v) { u.AutoExposureMinEv = v; },
                4.0f, "Auto mode: brightest the metering may expose to (lower EV = more brightening in dark scenes).", extras);
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Auto Max EV", cam->AutoExposureMaxEv, w, e, n,
                undo, "Change Auto Max EV",
                [](Components::Camera& u, float v) { u.AutoExposureMaxEv = v; },
                18.0f, "Auto mode: darkest the metering may expose to (higher EV = more darkening in bright scenes).", extras);
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Auto Speed Up", cam->AutoExposureSpeedUp, w, e, n,
                undo, "Change Auto Speed Up",
                [](Components::Camera& u, float v) { u.AutoExposureSpeedUp = std::max(0.0f, v); },
                1.0f, "Auto mode: rate the exposure RISES (the scene darkened, image brightens). Higher = faster.", extras);
            AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Auto Speed Down", cam->AutoExposureSpeedDown, w, e, n,
                undo, "Change Auto Speed Down",
                [](Components::Camera& u, float v) { u.AutoExposureSpeedDown = std::max(0.0f, v); },
                3.0f, "Auto mode: rate the exposure FALLS (the scene brightened, image darkens). Higher = faster.", extras);
        }

        // Lens controls remain visible in every exposure mode. Aperture is a
        // physical property of the lens: it affects DoF in every mode and also
        // contributes to exposure when Exposure Mode is Physical.
        InspectorUI::AddTextBlock(ctx.Parent, "Lens / Physical DoF", "inspector-section-subheader");
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Aperture (f)",
            std::max(cam->Aperture, Components::Camera::kApertureMin), w, e, n,
            undo, "Change Lens Aperture",
            [](Components::Camera& u, float v) { u.Aperture = std::max(Components::Camera::kApertureMin, v); },
            16.0f, "Lens f-number. Lower values produce shallower physical DoF; Physical exposure also becomes brighter.",
            extras, Components::Camera::kApertureMin);
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Focus Distance",
            std::max(cam->FocusDistance, Components::Camera::kFocusDistanceMin), w, e, n,
            undo, "Change Focus Distance",
            [](Components::Camera& u, float v) {
                u.FocusDistance = std::max(Components::Camera::kFocusDistanceMin, v);
            },
            10.0f, "Lens focus-plane distance in world units. Physical DoF (DepthOfFieldEffect on a volume) focuses here.",
            extras, Components::Camera::kFocusDistanceMin);
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Focus Debug Blend", cam->FocusDebugAlpha, w, e, n,
            undo, "Change Focus Debug Blend",
            [](Components::Camera& u, float v) { u.FocusDebugAlpha = std::clamp(v, 0.0f, 1.0f); },
            0.5f, "How much the camera's focus-range diagnostic tint is mixed over the image.", extras, 0.0f, 1.0f);
        AddToggleRow(ctx.Parent, "Focus Range Debug", cam->FocusDebugMode != 0,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<Components::Camera>(w, e, n, undo, "Change Focus Range Debug",
                    [v](Components::Camera& u) { u.FocusDebugMode = v ? 1 : 0; });
            },
            "Overlays green where this camera is in focus, red for far blur, and blue for near blur.");
        IntField* apertureBlades = AddComponentIntRowWithDrag<Components::Camera>(ctx.Parent, "Aperture Blades",
            std::clamp(static_cast<int>(cam->ApertureBladeCount),
                       static_cast<int>(Components::Camera::kApertureBladeCountMin),
                       static_cast<int>(Components::Camera::kApertureBladeCountMax)), w, e, n, undo,
            "Change Aperture Blade Count",
            [](Components::Camera& u, int v) {
                u.ApertureBladeCount = static_cast<uint32>(std::clamp(
                    v, static_cast<int>(Components::Camera::kApertureBladeCountMin),
                    static_cast<int>(Components::Camera::kApertureBladeCountMax)));
            },
            7, "Number of iris blades shaping out-of-focus highlights (3-16).", extras);
        apertureBlades->SetRange(static_cast<int>(Components::Camera::kApertureBladeCountMin),
                                 static_cast<int>(Components::Camera::kApertureBladeCountMax));
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Aperture Roundness", cam->ApertureRoundness, w, e, n,
            undo, "Change Aperture Roundness",
            [](Components::Camera& u, float v) { u.ApertureRoundness = std::clamp(v, 0.0f, 1.0f); },
            1.0f, "Shape of physical DoF bokeh: 0 = straight polygonal blades, 1 = circular aperture.", extras, 0.0f, 1.0f);
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Aperture Rotation", cam->ApertureRotation, w, e, n,
            undo, "Change Aperture Rotation",
            [](Components::Camera& u, float v) { u.ApertureRotation = std::clamp(v, 0.0f, 360.0f); },
            0.0f, "Rotation of the polygonal iris and bokeh shape in degrees.", extras, 0.0f, 360.0f);
        AddComponentFloatRowWithDrag<Components::Camera>(ctx.Parent, "Anamorphic Squeeze", cam->AnamorphicSqueeze, w, e, n,
            undo, "Change Anamorphic Squeeze",
            [](Components::Camera& u, float v) { u.AnamorphicSqueeze = std::clamp(v, 1.0f, 4.0f); },
            1.0f, "Horizontal lens squeeze. 1 = spherical; common anamorphic lenses use 1.33, 1.5, 1.8, or 2.0. Widens horizontal FOV and elongates bokeh vertically.", extras, 1.0f, 4.0f);

        // Film gate / sensor format. The preset writes SensorHeightMm; editing the
        // height by hand flips the preset to Custom. Sensor height scales both the
        // derived focal length and the DoF circle of confusion (bigger gate =
        // shallower focus at the same field of view).
        auto* sensorField = InspectorUI::AddEnumRow(ctx.Parent, "Sensor Format", kSensorPresets, cam->SensorPreset,
            "Film gate / sensor format preset. Sets the vertical gate height driving physical DoF: "
            "8/16 mm = deep focus, 65 mm / IMAX / medium format = shallow. Custom keeps the height editable below.");
        sensorField->SetOnValueChanged([w, e, n, undo, extras](Components::CameraSensorPreset v) {
            CommitComponentWithUndo<Components::Camera>(w, e, n, undo, "Change Sensor Format",
                [v](Components::Camera& u) {
                    u.SensorPreset = v;
                    if (v != Components::CameraSensorPreset::Custom)
                        u.SensorHeightMm = Components::CameraSensorPresetHeightMm(v);
                });
            if (n)
            {
                n->NotifyComponentChange<Components::Camera>(w, e, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
                for (auto& ex : extras)
                    n->NotifyComponentChange<Components::Camera>(w, ex, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
            }
        });
        auto sensorHeightHandlers = MakeComponentInteractiveHandlers<Components::Camera, float>(
            w, e, n, undo, "Change Sensor Height",
            [](Components::Camera& u, float v) {
                u.SensorHeightMm = std::clamp(v, 1.0f, 100.0f);
                u.SensorPreset = Components::CameraSensorPreset::Custom;
            },
            extras);
        const auto syncFocalLengthForSensor = [w, e, focalLengthFieldSlot]() {
            const auto* camera = w->GetComponent<Components::Camera>(e);
            FloatField* field = *focalLengthFieldSlot;
            if (camera && field)
                field->SetValueWithoutNotify(
                    Components::CameraFocalLengthMmFromVerticalFov(
                        camera->FovY, camera->SensorHeightMm));
        };
        AddFloatRowWithDrag(
            ctx.Parent, "Sensor Height (mm)", cam->SensorHeightMm,
            [preview = std::move(sensorHeightHandlers.first), syncFocalLengthForSensor](float v) {
                preview(v);
                syncFocalLengthForSensor();
            },
            [commit = std::move(sensorHeightHandlers.second), syncFocalLengthForSensor](float v) {
                commit(v);
                syncFocalLengthForSensor();
            },
            24.0f, "Vertical gate/sensor height in millimeters. Editing switches the format to Custom.",
            1.0f, 100.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::Camera>(std::move(fn));
}

} // namespace GameEngine
