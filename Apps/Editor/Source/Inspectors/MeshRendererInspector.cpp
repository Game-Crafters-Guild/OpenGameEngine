#include "Inspectors/MeshRendererInspector.h"

#include "InspectorRegistry.h"

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Materials/MaterialDocument.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/MaterialInspector.h"
#include "Inspectors/MaterialInspectorSections.h"
#include "Assets/AssetCreation.h"
#include "Editor/EditorPaths.h"
#include "UI/Controls/Button.h"
#include "UI/InspectorSection.h"
#include "UI/StyleProperties.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UndoRedo/UndoRedoService.h"

#include "Editor/Entities/EditorInspectorPseudoComponents.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace GameEngine
{

using InspectorUI::AddLine;

namespace
{

static void ApplyMaterialGuidToMeshRenderer(ECS::World* world,
                                           ECS::EntityHandle entity,
                                           const GUID& guid,
                                           Editor::EditorChangeNotifications* notifications)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;
    auto* mr = world->GetComponent<Components::MeshRenderer>(entity);
    if (!mr)
        return;

    Components::MeshRenderer updated = *mr;
    updated.materialAssetGuid.Set(guid);
    Editor::CommitComponentUpdate(world, entity, notifications, updated);
}

// Resolve the file-backed `MaterialAsset` for an entity's submesh, without
// blocking on async loads. Two component-GUID cases:
//   (a) `mr.materialAssetGuid` points to a loaded .material — return it.
//   (b) Derived runtime GUID — `AssetManager` has no record. Compute the
//       expected file path `<modelDir>/Materials/<slotName>.material` and
//       resolve that.
// Returns:
//   - `mat` non-null when fully resolved (cached and ready).
//   - `pendingLoadGuid` non-null when a sync result wasn't available but
//     scheduling a load on that GUID will (eventually) make the next call
//     succeed. The caller is expected to render a "Loading…" placeholder
//     and schedule `LoadAsset(pendingLoadGuid, callback)`; the callback
//     should dirty-mark the inspector so this function re-runs.
//   - Both null when the entity has no resolvable material at all (e.g.
//     primitive `DefaultMaterialGuid` with no on-disk backing).
struct ResolveActiveMaterialResult
{
    SharedPtr<MaterialAsset> mat;
    GUID resolvedGuid = GUID::Null();
    GUID pendingLoadGuid = GUID::Null();
};

static ResolveActiveMaterialResult
ResolveActiveMaterialAsset(const Components::MeshRenderer& mr, AssetManager& am)
{
    ResolveActiveMaterialResult out;

    const GUID componentGuid = mr.materialAssetGuid.ToGuid();
    if (componentGuid.IsNull())
        return out;

    am.ClearLoadSuppressed(componentGuid);

    auto cachedAs = [&am](const GUID& g) -> SharedPtr<MaterialAsset>
    {
        SharedPtr<Asset> a = am.GetAsset(g);
        return a ? std::dynamic_pointer_cast<MaterialAsset>(a) : nullptr;
    };

    if (auto direct = cachedAs(componentGuid))
    {
        out.mat = direct;
        out.resolvedGuid = componentGuid;
        return out;
    }

    // Component GUID is file-backed but not yet cached — schedule it.
    if (AssetMetadata md{}; am.GetRegistry().TryGetAssetMetadata(componentGuid, md))
    {
        out.pendingLoadGuid = componentGuid;
        return out;
    }

    // Fallback: derive .material file path from the model directory.
    const GUID modelGuid = mr.modelAssetGuid.ToGuid();
    if (modelGuid.IsNull())
        return out;

    AssetMetadata modelMd{};
    if (!am.GetRegistry().TryGetAssetMetadata(modelGuid, modelMd))
        return out;

    // Need the model loaded to look up the slot's material name.
    SharedPtr<Asset> modelAsset = am.GetAsset(modelGuid);
    if (!modelAsset)
    {
        // Model not cached — caller should schedule a load on it. Once the
        // model is in cache, the next inspector rebuild lets us look up the
        // expected .material file and schedule the second-stage load.
        out.pendingLoadGuid = modelGuid;
        return out;
    }

    auto* model = dynamic_cast<ModelAsset*>(modelAsset.get());
    if (!model || mr.meshId >= model->GetMeshCount())
        return out;

    const uint32 slotIdx = model->GetMesh(mr.meshId).MaterialIndex;
    if (slotIdx >= model->GetMaterialCount())
        return out;

    const auto& importedMat = model->GetMaterial(slotIdx);
    const std::string slotName = importedMat.Name.empty()
        ? (modelMd.Path.stem().string() + "_material_" + std::to_string(slotIdx))
        : std::string(importedMat.Name.c_str());
    const std::filesystem::path matPath = modelMd.Path.parent_path() / "Materials" / (slotName + ".material");

    std::error_code ec;
    if (!std::filesystem::exists(matPath, ec))
        return out;

    // Resolve through AssetManager, which registers the sibling Materials/ file
    // when nothing has yet: a raw registry lookup finds nothing for an
    // unregistered path.
    const GUID fileGuid = am.ResolveAssetGuid(matPath);
    if (fileGuid.IsNull())
        return out;

    if (auto fileMat = cachedAs(fileGuid))
    {
        out.mat = fileMat;
        out.resolvedGuid = fileGuid;
        return out;
    }

    out.pendingLoadGuid = fileGuid;
    return out;
}

// Load a model asset by GUID, register its GPU meshes and converted materials,
// then update the MeshRenderer component and ensure all rendering prerequisites
// (Transform, WorldTransform, LocalBounds, MeshGPUData) are present.
static void AssignModelToMeshRenderer(ECS::World* world,
                                      ECS::EntityHandle entity,
                                      const GUID& guid,
                                      Editor::EditorChangeNotifications* notifications)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;

    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;

    auto* mr = world->GetComponent<Components::MeshRenderer>(entity);
    if (!mr)
        return;

    // Drag-drop assigns the model's first submesh to this entity. Register
    // only that submesh's material slot — registering all model materials
    // here would upload textures + compile PSOs for slots that no entity
    // in the scene currently uses.
    auto resources = Engine::Renderer::RegisterModelRenderResourcesForSubmesh(*rs, guid, 0);
    if (!resources)
        return;

    // Update MeshRenderer with GPU mesh handle and material GUID. Drag-drop
    // reassigns the model, so the material follows the model (FromModel).
    Components::MeshRenderer updated = *mr;
    Engine::Renderer::PopulateMeshRenderer(updated, *resources, 0,
                                           Engine::Renderer::MeshMaterialFill::FromModel);
    world->AddComponentImmediate(entity, updated);

    // Ensure Transform exists (required for WorldTransform derivation).
    if (!world->GetComponent<Components::Transform>(entity))
    {
        Components::Transform xf{};
        world->AddComponentImmediate(entity, xf);
    }

    // Ensure WorldTransform exists (required by RenderExtractionSystem query).
    if (!world->GetComponent<Components::WorldTransform>(entity))
    {
        Components::WorldTransform wt{};
        world->AddComponentImmediate(entity, wt);
    }

    // This entity draws submesh 0, so its culling envelope is that submesh's, as
    // ModelEntityFactory and the reload refresh already derive it. A mesh the
    // upload refused publishes no entry; the whole-model box is the only envelope
    // left, and it still bounds the geometry the entity would draw.
    Components::LocalBounds bounds{};
    const auto* meshEntry = resources->meshHandles.empty() ? nullptr
        : rs->GetMeshGPURegistry().Find(resources->meshHandles[0]);
    if (meshEntry)
        bounds.Box = meshEntry->bounds;
    else
        bounds = Engine::Renderer::ComputeModelBounds(*resources->modelAsset);
    world->AddComponentImmediate(entity, bounds);

    // Reset stale MeshGPUData so the extraction system re-initializes GPU scene indices.
    auto* meshGpu = world->GetComponent<Components::MeshGPUData>(entity);
    if (meshGpu)
    {
        Components::MeshGPUData reset{};
        world->AddComponentImmediate(entity, reset);
    }

    if (notifications)
        notifications->NotifyComponentCommit<Components::MeshRenderer>(world, entity);

    // Export .material files next to the model so they're editable in the inspector.
    auto& am = EngineCore::GetInstance().GetAssetManager();
    AssetMetadata md{};
    if (am.GetRegistry().TryGetAssetMetadata(guid, md) && !md.Path.empty())
    {
        std::vector<ECS::EntityHandle> entities = {entity};
        Editor::ExportModelMaterials(md.Path, guid, entities, *world, am);
    }
}

static uint32_t BaseColorToIconTint(const MaterialDocument& doc)
{
    auto it = doc.properties.find("baseColor");
    if (it == doc.properties.end() || !std::holds_alternative<std::vector<float>>(it->second))
        return 0u;
    const auto& c = std::get<std::vector<float>>(it->second);
    if (c.size() < 3)
        return 0u;
    // Blend 50% towards white so dark materials still produce a visible tint.
    auto toU8 = [](float v) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return 0xFF000000u | (toU8(c[0]) << 16) | (toU8(c[1]) << 8) | toU8(c[2]);
}

static void BuildMeshRendererMaterialInspectorUI(const InspectorContext& ctx)
{
    if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        return;

    auto* mr = ctx.World->GetComponent<Components::MeshRenderer>(ctx.Entity);
    if (!mr)
        return;

    using namespace InspectorDrag;

    auto getWorld = ctx.GetWorld;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    auto extras = InspectorDrag::GetAdditionalEntities(ctx);
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();

    // ---- Material ----
    // The MeshRenderer component carries the assigned material directly
    // (`materialAssetGuid`). Read from the component — do NOT load the
    // ModelAsset to enumerate sibling material slots. Each submesh entity
    // owns its own MeshRenderer with its own material; editing model-wide
    // material lists belongs on the Model asset's inspector, not here.
    {
        const GUID currentMaterial = mr->materialAssetGuid.ToGuid();
        auto& am = EngineCore::GetInstance().GetAssetManager();

        // Header row: "Material" label + "New Material" button (creates a
        // fresh .material file in Assets/Materials and assigns it).
        {
            UIElement* headerRow = InspectorUI::AddRow(ctx.Parent);
            InspectorUI::AddLabel(headerRow, "Material");

            auto newMatBtn = std::make_unique<Button>();
            newMatBtn->AddClass("inspector-button");
            newMatBtn->AddClass("inspector-new-material-button");
            newMatBtn->SetText("New Material");
            newMatBtn->RegisterEventHandler(kEventButtonClick, [getWorld, e, n, undo, extras](UIEvent&)
                {
                    auto* w = getWorld ? getWorld() : nullptr;
                    if (!w) return;

                    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
                    if (projectPaths.workspaceRoot.empty())
                        return;
                    std::filesystem::path dir = projectPaths.workspaceRoot / "Assets" / "Materials";

                    auto& assetMgr = EngineCore::GetInstance().GetAssetManager();
                    auto result = Editor::CreateDefaultMaterialFile(dir, "NewMaterial", undo, &assetMgr);
                    if (result.path.empty())
                        return;

                    // Resolve through AssetManager, which registers the just-created
                    // file when nothing has yet: a raw registry lookup finds nothing for
                    // an unregistered path, and the new material would never be assigned.
                    const GUID newGuid = assetMgr.ResolveAssetGuid(result.path);
                    if (newGuid.IsNull())
                        return;

                    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
                    {
                        MaterialDocument doc = MaterialDocument::CreateDefaultPBR(
                            result.path.stem().string());
                        rs->RegisterAndPrewarmMaterial(newGuid, doc);
                    }

                    std::optional<Editor::UndoRedoService::InteractiveEdit> edit;
                    if (undo)
                    {
                        auto target = MakeMultiComponentSnapshotTarget<Components::MeshRenderer>(
                            w, e, extras, n, "Assign Material");
                        edit.emplace(undo->BeginInteractiveEdit("Assign Material", std::move(target)));
                    }

                    ApplyMaterialGuidToMeshRenderer(w, e, newGuid, n);
                    for (auto& ex : extras)
                        ApplyMaterialGuidToMeshRenderer(w, ex, newGuid, n);

                    // Force the inspector to rebuild so the AssetField picks up the
                    // new GUID and the inline Material Properties foldout appears.
                    // A plain Commit is skipped by the inspector's change subscription
                    // when the component signature is unchanged.
                    if (n)
                    {
                        n->NotifyComponentChange<Components::MeshRenderer>(
                            w, e, Editor::EditorChangeNotifications::ChangeKind::UndoRedo);
                        for (auto& ex : extras)
                            n->NotifyComponentChange<Components::MeshRenderer>(
                                w, ex, Editor::EditorChangeNotifications::ChangeKind::UndoRedo);
                    }

                    if (edit)
                        edit->Commit();
                });
            UIElement* headerFieldContainer = InspectorUI::AddFieldContainer(headerRow);
            headerFieldContainer->AddChild(std::move(newMatBtn));
        }

        // Resolve once: handles both file-backed `currentMaterial` and
        // derived runtime GUIDs (looked up via the model directory).
        // Returns synchronously without blocking on async loads. When a load
        // is needed, `pendingLoadGuid` is set and we schedule it below; the
        // callback dirties the inspector so this rebuilds with the loaded
        // asset. Until then, the foldout shows a "Loading…" placeholder.
        auto resolved = ResolveActiveMaterialAsset(*mr, am);
        const SharedPtr<MaterialAsset>& resolvedMat = resolved.mat;
        const GUID displayGuid = !resolved.resolvedGuid.IsNull()
            ? resolved.resolvedGuid
            : currentMaterial;

        std::shared_ptr<AssetLoadHandle> pendingLoad;
        if (!resolvedMat && !resolved.pendingLoadGuid.IsNull())
        {
            // Keep the handle with this section's frame callback so completion
            // can be observed for both successful and failed loads. Discarding
            // it leaves no terminal signal and turns a failure into a permanent
            // per-frame cache poll.
            pendingLoad = std::make_shared<AssetLoadHandle>(
                EngineCore::GetInstance().GetAssetManager().LoadAsset(
                    resolved.pendingLoadGuid,
                    AssetLoadResultCallback{},
                    AssetLoadPriority::High));
        }

        UIElement* matRow = InspectorUI::AddRow(ctx.Parent);
        InspectorUI::AddLabel(matRow, "Material");

        if (!displayGuid.IsNull() && ctx.PingAsset)
        {
            AssetMetadata matMd{};
            if (am.GetRegistry().TryGetAssetMetadata(displayGuid, matMd) && !matMd.Path.empty())
            {
                auto pingBtn = std::make_unique<Button>();
                pingBtn->AddClass("icon-button");
                pingBtn->AddClass("pin-icon");
                pingBtn->SetTooltip("Reveal Material in Assets");
                auto pingAsset = ctx.PingAsset;
                auto matPath = matMd.Path;
                pingBtn->RegisterEventHandler(kEventButtonClick, [pingAsset, matPath](UIEvent&) { pingAsset(matPath); });
                matRow->AddChild(std::move(pingBtn));
            }
        }

        UIElement* matFieldContainer = InspectorUI::AddFieldContainer(matRow);
        matFieldContainer->Overrides().Set(Style::FlexGrow, 1.0f);

        auto matField = std::make_unique<AssetField>();
        matField->AddClass("dropdown-asset-field");
        matField->SetAcceptedTypes({AssetType::Material});
        matField->SetAssetRegistry(&registry);
        if (ctx.Thumbnails)
            matField->SetThumbnailProvider(ctx.Thumbnails);
        matField->SetValue(displayGuid);
        matField->SetOnValueChanged(
            [getWorld, e, n, undo, extras](const GUID& guid) {
                auto* w = getWorld ? getWorld() : nullptr;
                if (!w) return;

                std::optional<Editor::UndoRedoService::InteractiveEdit> edit;
                if (undo)
                {
                    auto target = MakeMultiComponentSnapshotTarget<Components::MeshRenderer>(
                        w, e, extras, n, "Assign Material");
                    edit.emplace(undo->BeginInteractiveEdit("Assign Material", std::move(target)));
                }

                if (guid.IsNull())
                {
                    auto clearMat = [&](ECS::EntityHandle ent) {
                        auto* comp = w->GetComponent<Components::MeshRenderer>(ent);
                        if (!comp) return;
                        Components::MeshRenderer updated = *comp;
                        updated.materialAssetGuid.Clear();
                        w->AddComponentImmediate(ent, updated);
                    };
                    clearMat(e);
                    for (auto& ex : extras)
                        clearMat(ex);
                }
                else
                {
                    ApplyMaterialGuidToMeshRenderer(w, e, guid, n);
                    for (auto& ex : extras)
                        ApplyMaterialGuidToMeshRenderer(w, ex, guid, n);
                }

                if (edit)
                    edit->Commit();
            });
        matFieldContainer->AddChild(std::move(matField));

        // Inline material properties — uses the same resolution as the
        // AssetField above. Three states:
        //   - resolvedMat present  → render the foldout normally.
        //   - load in flight       → render a "Loading…" placeholder; the
        //                            scheduled LoadAsset above will dirty
        //                            the inspector when ready.
        //   - no material at all   → render nothing (quiet).
        if (resolvedMat)
        {
            if (ctx.Section)
            {
                const uint32_t tint = BaseColorToIconTint(resolvedMat->GetDocument());
                if (tint != 0u)
                    ctx.Section->SetHeaderIconTint(tint);
            }

            std::unique_ptr<Foldout> foldout = Editor::MakeMaterialPropertiesFoldout();
            auto openPicker = ctx.OpenColorPickerWindow;
            BuildMaterialInspectorUI(foldout->GetContentContainer(), resolvedMat.get(), openPicker, ctx.PingAsset,
                                     ctx.Undo, /*inlineEmbed=*/true, ctx.OpenAsset,
                                     ctx.PingAssetPreserveInspector);
            ctx.Parent->AddChild(std::move(foldout));
        }
        else if (!resolved.pendingLoadGuid.IsNull())
        {
            auto status = std::make_unique<Label>();
            Label* statusLabel = status.get();
            status->AddClass("inspector-text");
            status->SetText("Loading material…");
            ctx.Parent->AddChild(std::move(status));

            if (pendingLoad && ctx.FrameRefreshCallbacks && ctx.RequestInspectorRefresh)
            {
                UIElement* refreshHost = ctx.Parent;
                auto completed = std::make_shared<bool>(false);
                auto requestRefresh = ctx.RequestInspectorRefresh;
                ctx.FrameRefreshCallbacks->push_back(
                    [pendingLoad, refreshHost, statusLabel, completed, requestRefresh]()
                    {
                        if (*completed || !refreshHost || !pendingLoad->IsComplete())
                            return;

                        *completed = true;
                        if (pendingLoad->GetResult())
                        {
                            refreshHost->PostAction(requestRefresh);
                            return;
                        }

                        statusLabel->SetText("Failed to load material.");
                    });
            }
        }
    }

}

} // namespace

void RegisterMeshRendererInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* mr = ctx.World->GetComponent<Components::MeshRenderer>(ctx.Entity);
        if (!mr)
        {
            AddLine(ctx.Parent, "(MeshRenderer missing)");
            return;
        }

        using namespace InspectorDrag;

        auto getWorld = ctx.GetWorld;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        auto extras = InspectorDrag::GetAdditionalEntities(ctx);
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();

        // ---- Model asset field ----
        {
            const GUID currentModel = mr->modelAssetGuid.ToGuid();
            UIElement* modelRow = InspectorUI::AddRow(ctx.Parent);
            InspectorUI::AddLabel(modelRow, "Model");

            // Ping button before the asset field.
            if (!currentModel.IsNull() && ctx.PingAsset)
            {
                AssetMetadata modelMd{};
                if (registry.TryGetAssetMetadata(currentModel, modelMd) && !modelMd.Path.empty())
                {
                    auto pingBtn = std::make_unique<Button>();
                    pingBtn->AddClass("icon-button");
                    pingBtn->AddClass("link-icon");
                    auto pingAsset = ctx.PingAsset;
                    auto modelPath = modelMd.Path;
                    pingBtn->RegisterEventHandler(kEventButtonClick, [pingAsset, modelPath](UIEvent&) { pingAsset(modelPath); });
                    modelRow->AddChild(std::move(pingBtn));
                }
            }

            UIElement* modelFieldContainer = InspectorUI::AddFieldContainer(modelRow);
            modelFieldContainer->Overrides().Set(Style::FlexGrow, 1.0f);

            auto modelField = std::make_unique<AssetField>();
            modelField->AddClass("dropdown-asset-field");
            modelField->SetAcceptedTypes({AssetType::Model});
            modelField->SetAssetRegistry(&registry);
            if (ctx.Thumbnails)
                modelField->SetThumbnailProvider(ctx.Thumbnails);
            modelField->SetValue(currentModel);
            modelField->SetOnValueChanged(
                [getWorld, e, n, undo, extras](const GUID& guid) {
                    auto* w = getWorld ? getWorld() : nullptr;
                    if (!w) return;

                    std::optional<Editor::UndoRedoService::InteractiveEdit> edit;
                    if (undo)
                    {
                        auto target = MakeMultiComponentSnapshotTarget<Components::MeshRenderer>(
                            w, e, extras, n, "Assign Model");
                        edit.emplace(undo->BeginInteractiveEdit("Assign Model", std::move(target)));
                    }

                    if (guid.IsNull())
                    {
                        // Clear model reference.
                        auto clearModel = [&](ECS::EntityHandle ent) {
                            auto* comp = w->GetComponent<Components::MeshRenderer>(ent);
                            if (!comp) return;
                            Components::MeshRenderer updated = *comp;
                            updated.modelAssetGuid.Clear();
                            updated.meshGpuHandleId = 0;
                            Editor::CommitComponentUpdate(w, ent, n, updated);
                            if (w->GetComponent<Components::MeshGPUData>(ent))
                            {
                                Components::MeshGPUData reset{};
                                w->AddComponentImmediate(ent, reset);
                            }
                            if (w->GetComponent<Components::LocalBounds>(ent))
                            {
                                Components::LocalBounds lb{};
                                lb.Box = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
                                w->AddComponentImmediate(ent, lb);
                            }
                        };
                        clearModel(e);
                        for (auto& ex : extras)
                            clearModel(ex);
                    }
                    else
                    {
                        AssignModelToMeshRenderer(w, e, guid, n);
                        for (auto& ex : extras)
                            AssignModelToMeshRenderer(w, ex, guid, n);
                    }

                    if (edit)
                        edit->Commit();
                });
            modelFieldContainer->AddChild(std::move(modelField));
        }

        // ---- Editable fields ----
        ECS::World* w = ctx.World; // used only by helpers that take World* parameter
        InspectorDrag::AddComponentIntRowWithDrag<Components::MeshRenderer>(ctx.Parent, "MeshId", static_cast<int>(mr->meshId), w, e, n,
            [](Components::MeshRenderer& u, int v) { u.meshId = static_cast<uint32>(std::max(0, v)); },
            "Submesh index within the model asset", extras);

        InspectorDrag::AddComponentIntRowWithDrag<Components::MeshRenderer>(ctx.Parent, "RenderLayerMask", static_cast<int>(mr->renderLayerMask), w, e, n,
            [](Components::MeshRenderer& u, int v) { u.renderLayerMask = static_cast<uint32>(std::max(0, v)); },
            "Bitmask controlling which render layers include this mesh", extras);

        InspectorDrag::AddToggleRow(ctx.Parent, "CastShadows", mr->castShadows,
            [getWorld, e, n, extras](bool v) {
                auto* w = getWorld ? getWorld() : nullptr;
                if (!w) return;
                auto* comp = w->GetComponent<Components::MeshRenderer>(e);
                if (!comp) return;
                Components::MeshRenderer updated = *comp;
                updated.castShadows = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::MeshRenderer>(ex);
                    if (!c) continue;
                    Components::MeshRenderer u = *c;
                    u.castShadows = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            }, "Include this mesh in shadow depth passes");

        InspectorDrag::AddToggleRow(ctx.Parent, "ReceiveShadows", mr->receiveShadows,
            [getWorld, e, n, extras](bool v) {
                auto* w = getWorld ? getWorld() : nullptr;
                if (!w) return;
                auto* comp = w->GetComponent<Components::MeshRenderer>(e);
                if (!comp) return;
                Components::MeshRenderer updated = *comp;
                updated.receiveShadows = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::MeshRenderer>(ex);
                    if (!c) continue;
                    Components::MeshRenderer u = *c;
                    u.receiveShadows = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            }, "Sample shadow maps when shading this mesh");

        InspectorDrag::AddToggleRow(ctx.Parent, "MotionVectors", mr->motionVectors,
            [getWorld, e, n, extras](bool v) {
                auto* w = getWorld ? getWorld() : nullptr;
                if (!w) return;
                auto* comp = w->GetComponent<Components::MeshRenderer>(e);
                if (!comp) return;
                Components::MeshRenderer updated = *comp;
                updated.motionVectors = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::MeshRenderer>(ex);
                    if (!c) continue;
                    Components::MeshRenderer u = *c;
                    u.motionVectors = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            }, "Write per-pixel motion vectors for TAA / motion blur");
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::MeshRenderer>(std::move(fn));
    InspectorRegistry::Get().RegisterComponentInspectorByTypeId(
        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>(),
        BuildMeshRendererMaterialInspectorUI);
}

} // namespace GameEngine
