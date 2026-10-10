#include "Inspectors/ModelInspector.h"

#include "Assets/AuthoredLodImport.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ModelAsset.h"
#include "Assets/LodAssetSettings.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/ModelAssetSettings.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/MeshLODGeometry.h"
#include "AssetCore/AssetEvents.h"
#include "Core/Engine.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/RenderServices.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/ModelSettingsReload.h"
#include "Inspectors/SubmeshLodProvenance.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/InfoCard.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Toggle.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace
{

void ReloadModel(const std::filesystem::path& path);

void SaveAndReload(const std::filesystem::path& path, const Editor::FbxPerAssetImportSettings& settings)
{
    Editor::FbxPerAssetImportSettings toSave = settings;
    if (toSave.UseGlobalSettings)
        toSave.Settings = Editor::FbxImportSettings::Load(EngineCore::GetInstance().GetWorkspaceRoot());
    toSave.Save(path);
    ReloadModel(path);
}

void AddButton(UIElement* root, const std::string& text, std::function<void()> onClick, const char* tooltip = nullptr)
{
    if (!root)
        return;
    auto button = std::make_unique<Button>();
    button->SetText(text);
    if (tooltip)
        button->SetTooltip(tooltip);
    button->AddClass("inspector-button");
    button->RegisterEventHandler(kEventButtonClick, [onClick = std::move(onClick)](UIEvent&) {
        if (onClick)
            onClick();
    });
    root->AddChild(std::move(button));
}

// In-memory generation provenance: FNV-1a of the RESOLVED config last used to
// Generate an asset. Kept OUT of the authoritative .assetdb kv (store-contract
// A5) — the kv holds artist intent only; provenance is derived and would churn
// the git-tracked store on every Generate.
std::unordered_map<GUID, uint64_t>& GeneratedLodHashStore()
{
    static std::unordered_map<GUID, uint64_t> store;
    return store;
}

uint64_t HashResolvedLodConfig(const ResolvedLodSettings& r)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    auto mixFloat = [&mix](float f) {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        mix(bits);
    };
    mix(r.Generate ? 1u : 0u);
    mix(r.GenerateSkinned ? 1u : 0u);
    mix(r.Config.LodCount);
    for (int i = 0; i < 4; ++i)
    {
        mixFloat(r.Config.TargetRatios[i]);
        mixFloat(r.Config.TargetError[i]);
    }
    mix(static_cast<uint64_t>(r.Config.BorderRule));
    return h;
}

std::string FormatLodFloat(float value, int decimals = 3)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return std::string(buffer);
}

// The LOD selection configuration the renderer is currently running. The panel
// reports switch points, and a switch point only means anything against the
// mapping and budget actually in force -- both are live project settings. With
// no RenderServices (early startup, headless tooling) the constructed defaults
// stand, which is what a fresh renderer would hold.
struct LiveLodConfig
{
    Rendering::LodSelectionMode Mode = Rendering::LodSelectionMode::Sse;
    float ErrorBudgetPx = Rendering::kDefaultLodErrorBudgetPx;
    float SkinnedBudgetScale = Rendering::kDefaultLodSkinnedBudgetScale;
};

LiveLodConfig ReadLiveLodConfig()
{
    LiveLodConfig cfg;
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        cfg.Mode = rs->GetMeshGPURegistry().GetLodSelectionMode();
        cfg.ErrorBudgetPx = rs->GetLODErrorBudgetPx();
        cfg.SkinnedBudgetScale = rs->GetLODSkinnedBudgetScale();
    }
    return cfg;
}

// One line naming the configuration the switch points below belong to, so the
// numbers are never read against the wrong mapping.
std::string LodConfigSummary(const LiveLodConfig& cfg, bool tightClass)
{
    switch (cfg.Mode)
    {
        case Rendering::LodSelectionMode::Off:
            return "selection: off -- every submesh draws LOD0";
        case Rendering::LodSelectionMode::Coverage:
            return "selection: coverage (screen fraction)";
        case Rendering::LodSelectionMode::Sse:
            break;
    }
    std::string summary =
        "selection: screen-space error, budget " + FormatLodFloat(cfg.ErrorBudgetPx, 1) + "px";
    if (tightClass)
        summary += " x" + FormatLodFloat(cfg.SkinnedBudgetScale, 2) + " (skinned)";
    return summary;
}

// Any submesh with joints/weights => the model has skinned geometry.
bool ModelHasSkinnedMesh(const ModelAsset* model)
{
    const uint32 count = model->GetMeshCount();
    for (uint32 i = 0; i < count; ++i)
        if (model->GetMesh(i).IsSkinned())
            return true;
    return false;
}

using Editor::ClassifySubmeshLod;
using Editor::SubmeshLodProvenance;

bool AnyLodSlotConfigured(const AssetRegistry& registry, const std::filesystem::path& assetPath)
{
    for (uint32 slot = 1u; slot <= kMaxLodSlots; ++slot)
        if (!LoadLodSlotRef(registry, assetPath, slot).IsNull())
            return true;
    return false;
}

// Append a colored provenance pill to a submesh foldout header. `hasSlots` is the
// model-level explicit-LOD-slot kv presence — the only cheap slot signal available
// without re-resolving each slot per submesh, so an authored chain on a slotted
// model is labeled "authored (slot)" with a tooltip owning the imprecision.
void AddProvenanceBadge(UIElement* header, SubmeshLodProvenance provenance, bool hasSlots)
{
    if (!header)
        return;

    const char* text = "";
    const char* variant = "";
    const char* tip = "";
    switch (provenance)
    {
    case SubmeshLodProvenance::Lod0Only:
        text = "LOD0 only";
        variant = "lod0only";
        tip = "No simplified levels: this submesh renders at full resolution at every distance.";
        break;
    case SubmeshLodProvenance::Generated:
        text = "generated";
        variant = "generated";
        tip = "LODs simplified from LOD0 by meshoptimizer (index-only mid levels; "
              "the sloppy far level owns an attribute-honest vertex block).";
        break;
    case SubmeshLodProvenance::Authored:
        text = hasSlots ? "authored (slot)" : "authored";
        variant = "authored";
        tip = hasSlots
                  ? "Artist-authored LOD chain. This model has explicit LOD slots configured, so "
                    "the chain may draw from slot sources and/or in-file _LOD / MSFT_lod levels."
                  : "Artist-authored LOD chain from the source file (_LOD suffix or MSFT_lod).";
        break;
    case SubmeshLodProvenance::AuthoredDropped:
        text = "authored: LOD0 only";
        variant = "warn";
        tip = "This submesh has authored LOD geometry the GPU upload refuses, so it renders LOD0 "
              "at every distance. Three causes, each with its own fix. Per-level vertex blocks "
              "cannot yet be combined with morph targets — remove the blend shapes, or let this "
              "submesh use index-only levels, which are always admitted. A level whose positions "
              "are not finite or whose indices point past its own vertices drops the whole chain "
              "back to LOD0 — re-export that level. A level carrying no indices ends the chain "
              "there, so any level below it is dropped too — fill or remove the empty level. The "
              "list below shows the authored CPU chain, which is what the importer read, not what "
              "the renderer draws.";
        break;
    }

    auto badge = std::make_unique<Label>();
    badge->AddClass("inspector-provenance-badge");
    badge->AddClass(variant);
    badge->SetText(text);
    badge->SetTooltip(tip);
    header->AddChild(std::move(badge));
}

// Read-only per-LOD detail for one submesh (tris / achieved error or authored
// coverage / derived switch threshold) — the artist's clean-simplification window
// and closest proxy for spotting bad collapses. Built lazily on foldout expand.
void BuildSubmeshLodDetail(UIElement* content, const Mesh& mesh)
{
    const uint32 lodCount = std::min<uint32>(mesh.LODCount(), 4u);

    float error[4] = {0, 0, 0, 0};
    uint8_t sloppy[4] = {0, 0, 0, 0};
    for (uint32 k = 1; k < lodCount; ++k)
    {
        error[k]  = (k - 1u) < mesh.ExtraLODErrors.size() ? mesh.ExtraLODErrors[k - 1u] : 0.0f;
        sloppy[k] = (k - 1u) < mesh.ExtraLODSloppy.size() ? mesh.ExtraLODSloppy[k - 1u] : 0u;
    }
    float threshold[4] = {0, 0, 0, 0};
    // Mirror BuildGpuMeshRow's branch order under the LIVE selection mode, or
    // the panel reports switch points for a configuration the renderer is not
    // in. Off outranks everything and zeroes every switch point; otherwise an
    // authored chain with switch coverages seeds the thresholds directly (so
    // the panel shows the AUTHORED coverages); Coverage takes the whole-row
    // coverage mapping; and the SSE path derives from the achieved meshopt
    // errors, with sseSlotMask marking which slots came out SSE-normalized.
    const LiveLodConfig lodConfig = ReadLiveLodConfig();
    const bool authored = mesh.HasAuthoredLODs();
    const bool hasCoverage = !mesh.ExtraLODCoverage.empty();
    const bool selectionOff = lodConfig.Mode == Rendering::LodSelectionMode::Off;
    uint32 sseSlotMask = 0u;
    if (selectionOff)
        Rendering::DeriveLODThresholdsOff(4u, threshold);
    else if (hasCoverage)
        Rendering::ApplyAuthoredLODCoverage(mesh.ExtraLODCoverage.data(),
                                            static_cast<uint32>(mesh.ExtraLODCoverage.size()),
                                            lodCount, 4u, threshold);
    else if (lodConfig.Mode == Rendering::LodSelectionMode::Coverage)
        Rendering::DeriveLODThresholdsCoverage(
            error, sloppy, lodCount, authored,
            Rendering::MeshGPURegistry::kDefaultLODThresholds, 4u, threshold);
    else
    {
        // LOD0's box — the reference metric the GPU row's thresholds are derived
        // from, not the culling envelope the row's boundingRadius carries.
        // meshopt error is normalized to the max AABB axis, coverage to the
        // bounding-sphere radius.
        const Mathematics::BoundingBox bounds = Mathematics::BoundingBox::FromMinMax(
            {mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]},
            {mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]});
        const auto& he = bounds.halfExtents;
        const float maxExtent = 2.0f * std::max({he.x, he.y, he.z});
        sseSlotMask = Rendering::DeriveLODThresholds(
            error, sloppy, lodCount, authored, bounds.Radius(), maxExtent,
            Rendering::MeshGPURegistry::kDefaultLODThresholds, 4u, threshold);
    }
    // BuildGpuMeshRow tags a skinned chain carrying at least one SSE slot as
    // the tight class, and the scatter budgets that class at budget * scale --
    // so a character's switch sizes are not the ones the slider reads.
    const bool tightClass = sseSlotMask != 0u && mesh.IsSkinned();
    const float effectiveBudgetPx =
        tightClass ? lodConfig.ErrorBudgetPx * lodConfig.SkinnedBudgetScale
                   : lodConfig.ErrorBudgetPx;
    InspectorUI::AddTextBlock(content, LodConfigSummary(lodConfig, tightClass), "inspector-text");

    const size_t base = mesh.Indices.size() / 3;
    for (uint32 k = 0; k < lodCount; ++k)
    {
        const size_t tris = mesh.LODIndices(k).size() / 3;
        const int pct = base > 0
            ? static_cast<int>(std::lround(100.0 * static_cast<double>(tris) / static_cast<double>(base)))
            : 100;
        std::string line = "LOD" + std::to_string(k) + ": " + std::to_string(tris) +
                           " tris (" + std::to_string(pct) + "%)  ";
        if (k == 0)
        {
            line += "base";
        }
        else if (authored)
        {
            // Authored levels display their switch coverage (parallel to ExtraLODs);
            // achieved error is not meaningful for an artist-made block.
            const size_t cov = k - 1u;
            line += cov < mesh.ExtraLODCoverage.size()
                        ? "cov " + FormatLodFloat(mesh.ExtraLODCoverage[cov])
                        : "cov -";
            line += " (authored)";
        }
        else
        {
            line += "err " + FormatLodFloat(error[k]);
            line += sloppy[k] ? " (sloppy)" : " (generated)";
        }
        // With selection off every switch point is zeroed and the level never
        // engages, so there is no switch column to report -- the summary line
        // above says so once instead of repeating "0.000" per slot.
        if (k > 0 && !selectionOff)
        {
            // Each slot is shown in ITS OWN unit: an SSE-normalized slot
            // switches at a fixed on-screen size, so report that size in pixels
            // at the budget this mesh's class actually spends; a coverage-space
            // slot reports its coverage. The pixel size is what a viewport at
            // or above this mesh's break-even height switches at; a shorter view
            // pins to the coverage mapping instead (LodSseScaleCeil), which the
            // panel cannot name because it has no view context.
            if ((sseSlotMask >> k) & 1u)
                line += "  sw " +
                        std::to_string(static_cast<int>(
                            std::lround(2.0f * effectiveBudgetPx * threshold[k]))) +
                        "px";
            else
                line += "  thr " + FormatLodFloat(threshold[k]);
        }
        InspectorUI::AddTextBlock(content, line, "inspector-text");
    }
}

// A per-submesh header indicator the Preview-LOD slider updates live so each row
// reflects the clamped level the model-wide preview shows for that submesh.
struct SubmeshPreviewTarget
{
    uint32 LodCount = 1;      // this submesh's LOD count (clamp ceiling)
    Label* ActiveLabel = nullptr;
};

// Compact per-submesh summary text for a foldout header ("N LODs, lod0->coarse tris").
std::string FormatSubmeshCounts(const Mesh& mesh)
{
    const uint32 lodCount = std::min<uint32>(mesh.LODCount(), 4u);
    const size_t lod0Tris = mesh.Indices.size() / 3;
    if (lodCount <= 1u)
        return "1 LOD, " + std::to_string(lod0Tris) + " tri";
    const size_t coarseTris = mesh.LODIndices(lodCount - 1u).size() / 3;
    return std::to_string(lodCount) + " LODs, " + std::to_string(lod0Tris) + "->" +
           std::to_string(coarseTris) + " tri";
}

// Per-submesh results: one collapsible foldout per submesh. The header carries the
// submesh name, a compact count summary, a live preview-level indicator, and the
// provenance badge. Multi-submesh rows start collapsed and build their per-LOD
// detail lazily on first expand; a lone submesh stays open and builds it eagerly.
void BuildLodResultsPerSubmesh(UIElement* root, const ModelAsset* model, bool hasSlots,
                               std::shared_ptr<std::vector<SubmeshPreviewTarget>> previewTargets)
{
    const uint32 meshCount = model->GetMeshCount();
    uint32 maxLods = 1u;
    for (uint32 i = 0; i < meshCount; ++i)
        maxLods = std::max(maxLods, model->GetMesh(i).LODCount());
    if (maxLods <= 1u)
        return; // No submesh has LODs; the preview slider owns the "No LODs" message.

    InspectorUI::AddTextBlock(root, "LODs per submesh", "inspector-section-subheader");

    for (uint32 i = 0; i < meshCount; ++i)
    {
        const Mesh& mesh = model->GetMesh(i);
        const SubmeshLodProvenance provenance = ClassifySubmeshLod(mesh);

        auto foldoutOwned = std::make_unique<Foldout>();
        Foldout* foldout = foldoutOwned.get();
        foldout->AddClass("lod-submesh-foldout");
        foldout->SetTitle(mesh.Name.empty() ? ("Submesh " + std::to_string(i)) : std::string(mesh.Name));

        UIElement* header = foldout->GetHeader();
        if (header)
        {
            auto counts = std::make_unique<Label>();
            counts->AddClass("lod-submesh-counts");
            counts->SetText(FormatSubmeshCounts(mesh));
            header->AddChild(std::move(counts));

            auto active = std::make_unique<Label>();
            active->AddClass("lod-submesh-active");
            Label* activePtr = active.get();
            header->AddChild(std::move(active));
            previewTargets->push_back(
                {std::min<uint32>(mesh.LODCount(), 4u), activePtr});

            AddProvenanceBadge(header, provenance, hasSlots);
        }

        // Many submeshes: collapse and defer detail construction to first expand.
        // Re-fetch by index inside the builder so a later vector reallocation can
        // not dangle a captured Mesh reference; the model outlives the inspector.
        // A single submesh stays open, but the foldout starts expanded and the lazy
        // builder only fires on a collapse->expand transition, so that path would
        // never build — construct its detail eagerly into the content container.
        if (meshCount > 1u)
        {
            foldout->Collapse();
            foldout->SetLazyContentBuilder(
                [model, i](UIElement* contentContainer) {
                    BuildSubmeshLodDetail(contentContainer, model->GetMesh(i));
                });
        }
        else
        {
            BuildSubmeshLodDetail(foldout->GetContentContainer(), mesh);
        }

        root->AddChild(std::move(foldoutOwned));
    }
}

// Preview-LOD slider: forces a model-wide level on the thumbnail preview and drives
// the per-submesh live indicators (each clamped to its submesh's LOD count).
void BuildLodPreviewSlider(UIElement* root, ModelAsset* model,
                           std::shared_ptr<std::vector<SubmeshPreviewTarget>> previewTargets)
{
    const uint32 meshCount = model->GetMeshCount();
    uint32 maxLods = 1;
    for (uint32 i = 0; i < meshCount; ++i)
        maxLods = std::max(maxLods, model->GetMesh(i).LODCount());

    if (maxLods <= 1)
    {
        InspectorUI::AddTextBlock(root, "No LODs generated", "inspector-asset-type-line");
        ModelThumbnailHandler::SetPreviewForcedLOD(0xFFFFFFFFu);
        return;
    }

    std::vector<size_t> lodTris(maxLods, 0);
    for (uint32 lod = 0; lod < maxLods; ++lod)
        for (uint32 i = 0; i < meshCount; ++i)
        {
            const Mesh& m = model->GetMesh(i);
            if (lod < m.LODCount())
                lodTris[lod] += m.LODIndices(lod).size() / 3;
        }

    auto readout = std::make_unique<Label>();
    readout->AddClass("inspector-asset-type-line");
    Label* readoutPtr = readout.get();
    root->AddChild(std::move(readout));

    auto formatReadout = [lodTris](int lod) {
        const size_t base = lodTris.empty() ? 0 : lodTris[0];
        const size_t tris = (lod >= 0 && lod < static_cast<int>(lodTris.size())) ? lodTris[lod] : 0;
        const int pct = base > 0
            ? static_cast<int>(std::lround(100.0 * static_cast<double>(tris) / static_cast<double>(base)))
            : 100;
        return "LOD " + std::to_string(lod) + ": " + std::to_string(tris) +
               " tris (" + std::to_string(pct) + "% of LOD0)";
    };

    // Reflect a model-wide preview level on each submesh's header indicator, clamped
    // to the submesh's own LOD count (a submesh with fewer levels stays at its
    // coarsest). Empty at LOD0 so 26-submesh headers are not cluttered.
    auto updatePreviewTargets = [previewTargets](int lod) {
        if (!previewTargets)
            return;
        for (SubmeshPreviewTarget& target : *previewTargets)
        {
            if (!target.ActiveLabel)
                continue;
            if (lod <= 0)
            {
                target.ActiveLabel->SetText("");
                target.ActiveLabel->RemoveClass("clamped");
                continue;
            }
            const int clamped = std::min<int>(lod, static_cast<int>(target.LodCount) - 1);
            target.ActiveLabel->SetText("shows LOD" + std::to_string(clamped));
            if (clamped < lod)
                target.ActiveLabel->AddClass("clamped");
            else
                target.ActiveLabel->RemoveClass("clamped");
        }
    };

    ModelThumbnailHandler::SetPreviewForcedLOD(0u);
    readoutPtr->SetText(formatReadout(0));

    const int maxLod = static_cast<int>(maxLods) - 1;
    auto sliderRow = InspectorDrag::AddSliderWithIntValueRow(
        root, "Preview LOD", 0, 0, maxLod,
        "Switch the model preview between LOD levels; per-submesh rows show the clamped level.");
    Slider* slider = sliderRow.Slider;
    IntField* valueField = sliderRow.ValueField;
    if (slider && valueField)
    {
        slider->SetStep(1.0f);
        slider->SetShowValueBubble(false);
        auto apply = [readoutPtr, formatReadout, updatePreviewTargets, slider, valueField, maxLod](float raw) {
            const int lod = std::clamp(static_cast<int>(std::lround(raw)), 0, maxLod);
            slider->SetValueWithoutNotify(static_cast<float>(lod));
            valueField->SetValueWithoutNotify(lod);
            ModelThumbnailHandler::SetPreviewForcedLOD(static_cast<uint32_t>(lod));
            if (readoutPtr)
                readoutPtr->SetText(formatReadout(lod));
            updatePreviewTargets(lod);
        };
        slider->SetOnValueChanging([apply](const float& v) mutable { apply(v); });
        slider->SetOnValueChanged([apply](const float& v) mutable { apply(v); });
        valueField->SetOnValueChanging([apply](const int& v) mutable { apply(static_cast<float>(v)); });
        valueField->SetOnValueChanged([apply](const int& v) mutable { apply(static_cast<float>(v)); });
    }
}

// Per-model LOD section: config foldout, explicit Generate (resolved config),
// staleness hint, results table, then the kept Preview-LOD slider.
// Phase C2a explicit LOD slots: slot1..3 each name another model whose LOD0
// geometry becomes this model's LOD 1..3. Persisted as an AssetRef kv; a change
// reloads the asset so ModelAsset::PostLoad re-resolves the authored chain. Full
// per-submesh provenance UI is C3 — this is the minimal read/write surface.
void BuildExplicitLodSlotSection(UIElement* root, const std::filesystem::path& assetPath,
                                 std::function<void()> requestInspectorRefresh)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    InspectorUI::AddTextBlock(root, "Explicit LOD slots", "inspector-section-subheader");

    for (uint32 slot = 1u; slot <= kMaxLodSlots; ++slot)
    {
        const GUID current = LoadLodSlotRef(assets.GetRegistry(), assetPath, slot);
        InspectorUI::AddAssetFieldRow(
            root, "LOD" + std::to_string(slot) + " source", current,
            {AssetType::Model}, &assets.GetRegistry(),
            [assetPath, slot, requestInspectorRefresh](const GUID& picked) {
                auto& am = EngineCore::GetInstance().GetAssetManager();
                // Persist a project-relative path hint alongside the GUID so the kv
                // reads meaningfully in a git diff (the GUID stays authoritative on load).
                std::string pathHint;
                if (!picked.IsNull())
                {
                    AssetMetadata meta;
                    if (am.GetRegistry().TryGetAssetMetadata(picked, meta))
                        AssetRegistry::TryComputeCanonicalRelativePath(am.GetAssetRoot(), meta.Path, pathHint);
                }
                SaveLodSlotRef(am.GetRegistry(), assetPath, slot, picked, pathHint);
                ReloadModel(assetPath);
                if (requestInspectorRefresh)
                    requestInspectorRefresh();
            },
            nullptr,
            "Another model asset whose LOD0 geometry becomes this model's LOD at this "
            "slot (per-submesh, matched by name). Slots layer under in-file _LOD chains.");
    }
}

void BuildLodSection(UIElement* root, ModelAsset* model, std::function<void()> requestInspectorRefresh)
{
    const std::filesystem::path assetPath = model->GetPath();
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const GUID guid = assets.ResolveAssetGuid(assetPath);

    InspectorUI::AddTextBlock(root, "Generation", "inspector-section-subheader");

    const LodAssetSettings settings = LodAssetSettings::Load(assets.GetRegistry(), assetPath);
    const ResolvedLodSettings resolved = ResolveLodSettings(&assets, assetPath);

    // Persist a single field edit to kv WITHOUT regenerating (explicit Generate
    // applies it), then refresh so greyed states and the staleness hint update.
    auto persist = [assetPath, requestInspectorRefresh](const std::function<void(LodAssetSettings&)>& mutate) {
        auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        LodAssetSettings edited = LodAssetSettings::Load(registry, assetPath);
        mutate(edited);
        edited.Save(registry, assetPath);
        if (requestInspectorRefresh)
            requestInspectorRefresh();
    };

    InspectorDrag::AddToggleRow(root, "Use project defaults", settings.UseGlobal,
        [persist](bool v) { persist([v](LodAssetSettings& s) { s.UseGlobal = v; }); },
        "Inherit LOD generation settings from the project defaults");

    if (settings.UseGlobal)
    {
        InspectorUI::AddTextBlock(root,
            std::string("Auto-generate: ") + (resolved.Generate ? "on" : "off") + " (project default)",
            "inspector-asset-type-line");
    }
    else
    {
        InspectorDrag::AddToggleRow(root, "Auto-generate LODs", settings.Generate,
            [persist](bool v) { persist([v](LodAssetSettings& s) { s.Generate = v; }); },
            "Generate simplified LODs for this model on import/reload");

        InspectorDrag::AddIntRowWithDrag(root, "LOD Count", static_cast<int>(settings.LodCount),
            [](int) {},
            [persist](int v) { persist([v](LodAssetSettings& s) { s.LodCount = static_cast<uint32>(std::clamp(v, 1, 4)); }); },
            4, "Number of levels including LOD0 (1-4)");

        for (uint32 lod = 1; lod < std::clamp<uint32>(settings.LodCount, 1u, 4u); ++lod)
        {
            InspectorDrag::AddFloatRowWithDrag(root, "LOD" + std::to_string(lod) + " target %",
                settings.TargetRatios[lod], [](float) {},
                [persist, lod](float v) { persist([v, lod](LodAssetSettings& s) { s.TargetRatios[lod] = std::clamp(v, 0.0f, 1.0f); }); },
                settings.TargetRatios[lod], "Target fraction of LOD0 triangles", 0.0f, 1.0f);
            InspectorDrag::AddFloatRowWithDrag(root, "LOD" + std::to_string(lod) + " max error",
                settings.TargetError[lod], [](float) {},
                [persist, lod](float v) { persist([v, lod](LodAssetSettings& s) { s.TargetError[lod] = std::max(v, 0.0f); }); },
                settings.TargetError[lod], "Maximum relative geometric error", 0.0f, 1.0f);
        }

        {
            static const std::vector<Dropdown::Option> kBorderOptions = {
                {"0", "Seam planes"},
                {"1", "Lock all"},
                {"2", "Free"},
            };
            Dropdown* borderDropdown = InspectorUI::AddDropdownRow(
                root, "Border rule", kBorderOptions,
                static_cast<int>(settings.BorderRule),
                "Open-border handling during simplification: Seam planes locks only "
                "kit tiling seams (borders on the bounding-box faces), Lock all pins "
                "every open border, Free lets all borders drift within the error budget");
            borderDropdown->SetOnValueChanged([persist](const std::string& value) {
                const uint32 rule = value == "1" ? 1u : (value == "2" ? 2u : 0u);
                persist([rule](LodAssetSettings& s) {
                    s.BorderRule = static_cast<MeshLODBorderRule>(rule);
                });
            });
        }

        if (ModelHasSkinnedMesh(model))
        {
            InspectorDrag::AddToggleRow(root, "Generate for skinned meshes", settings.GenerateSkinned,
                [persist](bool v) { persist([v](LodAssetSettings& s) { s.GenerateSkinned = v; }); },
                "Experimental: simplify skinned meshes (position-only simplify can cause weight swim)");
        }
    }

    // Staleness hint: RESOLVED config changed since the last Generate. Silent
    // when provenance is unknown (never generated this session).
    const uint64_t currentHash = HashResolvedLodConfig(resolved);
    if (auto it = GeneratedLodHashStore().find(guid);
        it != GeneratedLodHashStore().end() && it->second != currentHash)
    {
        InspectorUI::AddTextBlock(root, "Settings changed - press Generate to apply",
                                  "inspector-asset-type-line");
    }

    if (!IsMeshLODGenerationAvailable())
    {
        InspectorUI::AddTextBlock(root, "(meshoptimizer not available)", "inspector-asset-type-line");
    }
    else
    {
        AddButton(root, "Generate LODs", [assetPath, model, guid, requestInspectorRefresh] {
            auto& assetManager = EngineCore::GetInstance().GetAssetManager();
            const ResolvedLodSettings r = ResolveLodSettings(&assetManager, assetPath);
            // Regenerate in place; the MeshGPURegistry content-hash check
            // re-uploads the changed LOD table under the same handle on reload.
            model->GenerateLODs(r.Config, r.GenerateSkinned);
            GeneratedLodHashStore()[guid] = HashResolvedLodConfig(r);
            if (!guid.IsNull())
                assetManager.GetEventDispatcher().DispatchEvent(
                    AssetEvent(AssetEventType::AssetReloaded, guid, AssetType::Model, assetPath.string()));
            if (requestInspectorRefresh)
                requestInspectorRefresh();
        }, "Generate LODs now using the resolved settings; fires a reload so the preview updates.");
    }

    BuildExplicitLodSlotSection(root, assetPath, requestInspectorRefresh);

    const bool hasSlots = AnyLodSlotConfigured(assets.GetRegistry(), assetPath);
    auto previewTargets = std::make_shared<std::vector<SubmeshPreviewTarget>>();
    BuildLodResultsPerSubmesh(root, model, hasSlots, previewTargets);
    BuildLodPreviewSlider(root, model, previewTargets);
}

void ReloadModel(const std::filesystem::path& path)
{
    auto& assets = EngineCore::GetInstance().GetAssetManager();
    Editor::ReloadModelForChangedSettings(assets, assets.ResolveAssetGuid(path), path);
}

enum class ModelInspectorTab : int
{
    Model = 0,
    Lod = 1,
    Rig = 2
};

ModelInspectorTab& SelectedModelInspectorTab(const std::filesystem::path& path)
{
    static std::unordered_map<std::string, ModelInspectorTab> store;
    return store[path.generic_string()];
}

std::string EncodeBakeCsv(const float deg[3])
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.4g,%.4g,%.4g",
                  static_cast<double>(deg[0]), static_cast<double>(deg[1]),
                  static_cast<double>(deg[2]));
    return buf;
}

std::string EncodeBakeAxesCsv(const bool enabled[3])
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d,%d,%d",
                  enabled[0] ? 1 : 0, enabled[1] ? 1 : 0, enabled[2] ? 1 : 0);
    return buf;
}

void PersistBakeAndReload(const std::filesystem::path& path,
                          const float deg[3],
                          const bool enabled[3],
                          const std::function<void()>& requestInspectorRefresh)
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    registry.SetMetaValue(path, FbxLoaderOptions::kBakeRotationKey, EncodeBakeCsv(deg));
    registry.SetMetaValue(path, FbxLoaderOptions::kBakeRotationAxesKey, EncodeBakeAxesCsv(enabled));
    ReloadModel(path);
    if (requestInspectorRefresh)
        requestInspectorRefresh();
}

void PersistMirrorAndReload(const std::filesystem::path& path,
                            const bool axes[3],
                            const std::function<void()>& requestInspectorRefresh)
{
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    registry.SetMetaValue(path, FbxLoaderOptions::kMirrorAxesKey, EncodeBakeAxesCsv(axes));
    ReloadModel(path);
    if (requestInspectorRefresh)
        requestInspectorRefresh();
}

void AddHintBox(UIElement* parent, const std::string& text)
{
    if (!parent)
        return;
    parent->AddChild(std::make_unique<EditorUI::CollapsibleInfoCard>(text));
}

void AddSubtabBar(UIElement* root,
                  const std::filesystem::path& assetPath,
                  ModelInspectorTab current,
                  const std::function<void()>& requestInspectorRefresh)
{
    auto bar = std::make_unique<UIElement>();
    bar->AddClass("inspector-subtab-row");
    UIElement* barRaw = bar.get();

    struct TabSpec
    {
        ModelInspectorTab Tab;
        const char* Label;
    };
    const TabSpec tabs[] = {
        {ModelInspectorTab::Model, "Model"},
        {ModelInspectorTab::Lod, "LOD"},
        {ModelInspectorTab::Rig, "Rig"},
    };
    for (const TabSpec& spec : tabs)
    {
        auto button = std::make_unique<Button>();
        button->SetText(spec.Label);
        button->SetId(std::string("model-inspector-tab-") + spec.Label);
        button->AddClass("inspector-subtab");
        if (spec.Tab == current)
            button->AddClass("active");
        const ModelInspectorTab tab = spec.Tab;
        button->RegisterEventHandler(kEventButtonClick,
            [assetPath, tab, requestInspectorRefresh](UIEvent&) {
                SelectedModelInspectorTab(assetPath) = tab;
                if (requestInspectorRefresh)
                    requestInspectorRefresh();
            });
        barRaw->AddChild(std::move(button));
    }
    root->AddChild(std::move(bar));
}

void BuildBakeRotationSection(UIElement* root,
                              const std::filesystem::path& assetPath,
                              const FbxLoaderOptions& fbxOpts,
                              std::function<void()> requestInspectorRefresh)
{
    InspectorUI::AddTextBlock(root, "Bake Rotation", "inspector-section-subheader");
    AddHintBox(root,
               "Applied when this model is imported. The source file is not rewritten. "
               "Toggling an axis or editing degrees reloads the model automatically; "
               "Reimport is only needed if you edited the file on disk.");

    auto bake = std::make_shared<std::array<float, 3>>();
    auto axes = std::make_shared<std::array<bool, 3>>();
    for (int i = 0; i < 3; ++i)
    {
        (*bake)[i] = fbxOpts.Axis.BakeRotationDeg[i];
        (*axes)[i] = fbxOpts.Axis.BakeRotationAxisEnabled[i];
    }

    const char* axisLabels[3] = {"X", "Y", "Z"};
    const char* axisTips[3] = {
        "Roll around engine +X (degrees). Off contributes 0 to the import bake.",
        "Yaw around engine +Y (degrees). 180 turns imported +Z around to -Z. Off contributes 0.",
        "Pitch around engine +Z (degrees). Off contributes 0 to the import bake.",
    };

    for (int axis = 0; axis < 3; ++axis)
    {
        UIElement* row = InspectorUI::AddRow(root);
        Label* label = InspectorUI::AddLabel(row, axisLabels[axis], axisTips[axis]);
        UIElement* field = InspectorUI::AddFieldContainer(row);
        field->AddClass("inspector-bake-axis-field");

        Toggle* toggle = InspectorUI::AddToggle(field, (*axes)[axis]);
        FloatField* degrees = InspectorUI::AddFloat(field, (*bake)[axis]);
        degrees->SetEnabled((*axes)[axis]);

        toggle->SetOnValueChanged(
            [assetPath, bake, axes, axis, requestInspectorRefresh](const bool& value) {
                (*axes)[axis] = value;
                PersistBakeAndReload(assetPath, bake->data(), axes->data(), requestInspectorRefresh);
            });
        degrees->SetOnValueChanging(
            [bake, axis](const float& v) { (*bake)[axis] = v; });
        degrees->SetOnValueChanged(
            [assetPath, bake, axes, axis, requestInspectorRefresh](const float& v) {
                (*bake)[axis] = v;
                PersistBakeAndReload(assetPath, bake->data(), axes->data(), requestInspectorRefresh);
            });
        InspectorDrag::SetupLabelDragFloat(
            label, degrees,
            [bake, axis, degrees]() { (*bake)[axis] = degrees->GetValue(); },
            [assetPath, bake, axes, axis, degrees, requestInspectorRefresh]() {
                (*bake)[axis] = degrees->GetValue();
                PersistBakeAndReload(assetPath, bake->data(), axes->data(), requestInspectorRefresh);
            },
            0.0f, -360.0f, 360.0f);
    }
}

void BuildMirrorSection(UIElement* root,
                        const std::filesystem::path& assetPath,
                        const FbxLoaderOptions& fbxOpts,
                        std::function<void()> requestInspectorRefresh)
{
    InspectorUI::AddTextBlock(root, "Mirror", "inspector-section-subheader");
    AddHintBox(root,
               "Engine-space axis mirrors after axis conversion, before bake rotation. "
               "X on is the FBX right-to-left-handed bake, the same as the glTF one. "
               "First resolve writes assets.fbx.mirrorAxes so this is never hidden.");

    auto axes = std::make_shared<std::array<bool, 3>>();
    for (int i = 0; i < 3; ++i)
        (*axes)[i] = fbxOpts.Axis.MirrorAxis[i];

    const char* axisLabels[3] = {"X", "Y", "Z"};
    const char* axisTips[3] = {
        "Mirror engine +X. On by default: FBX is right-handed, this engine is left-handed.",
        "Mirror engine +Y (up/down).",
        "Mirror engine +Z (front/back).",
    };
    for (int axis = 0; axis < 3; ++axis)
    {
        InspectorDrag::AddToggleRow(root, axisLabels[axis], (*axes)[axis],
            [assetPath, axes, axis, requestInspectorRefresh](bool value) {
                (*axes)[axis] = value;
                PersistMirrorAndReload(assetPath, axes->data(), requestInspectorRefresh);
            },
            axisTips[axis]);
    }
}

void BuildImportSettingsSection(UIElement* root,
                                const std::filesystem::path& assetPath,
                                std::function<void()> requestInspectorRefresh)
{
    InspectorUI::AddTextBlock(root, "Import", "inspector-section-subheader");

    auto settings = Editor::FbxPerAssetImportSettings::Load(assetPath);
    const auto workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    const auto global = Editor::FbxImportSettings::Load(workspaceRoot);
    if (settings.UseGlobalSettings)
        settings.Settings = global;

    InspectorDrag::AddToggleRow(root, "Use project defaults", settings.UseGlobalSettings,
        [assetPath, requestInspectorRefresh](bool value) {
            auto edited = Editor::FbxPerAssetImportSettings::Load(assetPath);
            edited.UseGlobalSettings = value;
            SaveAndReload(assetPath, edited);
            if (requestInspectorRefresh)
                requestInspectorRefresh();
        },
        "Use Project Settings > Asset Import defaults. Flag edits already reload the model.");

    if (!settings.UseGlobalSettings)
    {
        auto addAssetToggle = [root, assetPath, requestInspectorRefresh](const char* label,
                                                                         bool current,
                                                                         auto assign,
                                                                         const char* tooltip)
        {
            InspectorDrag::AddToggleRow(root, label, current,
                [assetPath, assign, requestInspectorRefresh = requestInspectorRefresh](bool value) {
                    auto edited = Editor::FbxPerAssetImportSettings::Load(assetPath);
                    edited.UseGlobalSettings = false;
                    assign(edited.Settings, value);
                    SaveAndReload(assetPath, edited);
                    if (requestInspectorRefresh)
                        requestInspectorRefresh();
                },
                tooltip);
        };

        addAssetToggle("Scene extras", settings.Settings.ImportSceneExtras,
            [](Editor::FbxImportSettings& s, bool v) { s.ImportSceneExtras = v; },
            "Spawn imported cameras, lights, and helper hierarchy");
        addAssetToggle("Cameras", settings.Settings.ImportCameras,
            [](Editor::FbxImportSettings& s, bool v) { s.ImportCameras = v; },
            "Spawn imported cameras");
        addAssetToggle("Lights", settings.Settings.ImportLights,
            [](Editor::FbxImportSettings& s, bool v) { s.ImportLights = v; },
            "Spawn imported lights");
        addAssetToggle("Helper nodes", settings.Settings.ImportHelperNodes,
            [](Editor::FbxImportSettings& s, bool v) { s.ImportHelperNodes = v; },
            "Rebuild authored helper/node hierarchy");
        addAssetToggle("Generate missing tangents", settings.Settings.GenerateMissingTangents,
            [](Editor::FbxImportSettings& s, bool v) { s.GenerateMissingTangents = v; },
            "Generate tangent data for normal-mapped FBX meshes that do not provide tangents");
        addAssetToggle("Clean skin weights", settings.Settings.CleanSkinWeights,
            [](Editor::FbxImportSettings& s, bool v) { s.CleanSkinWeights = v; },
            "Ask ufbx to remove invalid, zero, and negative skin weights before import");
        addAssetToggle("Adjust pivots", settings.Settings.AdjustPivots,
            [](Editor::FbxImportSettings& s, bool v) { s.AdjustPivots = v; },
            "Apply FBX pivot handling during import");
        addAssetToggle("Preserve geometry transforms", settings.Settings.PreserveGeometryTransforms,
            [](Editor::FbxImportSettings& s, bool v) { s.PreserveGeometryTransforms = v; },
            "Preserve authored geometry transforms instead of baking them into mesh data");
        addAssetToggle("Embedded textures", settings.Settings.ImportEmbeddedTextures,
            [](Editor::FbxImportSettings& s, bool v) { s.ImportEmbeddedTextures = v; },
            "Read embedded FBX image data when available");
    }

    AddButton(root, "Reimport FBX", [assetPath, requestInspectorRefresh] {
        SaveAndReload(assetPath, Editor::FbxPerAssetImportSettings::Load(assetPath));
        if (requestInspectorRefresh)
            requestInspectorRefresh();
    }, "Force a reload from the source file. Bake and import-flag edits already reload "
       "automatically; use this after changing the FBX on disk.");
}

void ApplyModelPreviewImage(UIElement& host, const std::string& relOrEngine)
{
    constexpr const char* kEnginePrefix = "engine:";
    constexpr size_t kEnginePrefixLen = 7;
    if (relOrEngine.empty())
        return;
    if (relOrEngine.rfind(kEnginePrefix, 0) == 0)
        UI::Layout::SetBackgroundResourceName(host, relOrEngine.substr(kEnginePrefixLen));
    else
        UI::Layout::SetBackgroundPath(host, relOrEngine);
}

void BuildOrientationPreview(UIElement* root,
                             const std::filesystem::path& assetPath,
                             IThumbnailProvider* thumbs)
{
    InspectorUI::AddTextBlock(root, "Default orientation", "inspector-section-subheader");
    AddHintBox(root,
               "Identity transform. Camera looks from +X/+Y/+Z, so a +Z-facing mesh "
               "points toward the camera. Bake, import-flag, and reimport edits reload this.");

    auto host = std::make_unique<UIElement>();
    host->AddClass("inspector-model-orientation-preview");
    host->SetId("inspector-model-orientation-preview");
    UIElement* hostRaw = host.get();
    root->AddChild(std::move(host));

    if (thumbs)
    {
        // Model thumbnails return an engine: id synchronously. Do not capture
        // hostRaw into onReady — inspector rebuilds free that pointer.
        const std::string immediate = thumbs->GetOrRequest(assetPath, 256, nullptr, true);
        if (!immediate.empty())
            ApplyModelPreviewImage(*hostRaw, immediate);
    }

    auto legend = std::make_unique<UIElement>();
    legend->AddClass("inspector-model-axis-legend");
    struct AxisSpec { const char* Axis; const char* Direction; const char* Class; };
    const AxisSpec axes[] = {
        {"+X", "Right", "inspector-model-axis-x"},
        {"+Y", "Up", "inspector-model-axis-y"},
        {"+Z", "Forward", "inspector-model-axis-z"},
    };
    for (const AxisSpec& spec : axes)
    {
        auto chip = std::make_unique<UIElement>();
        chip->AddClass("inspector-model-axis-chip");

        auto axis = std::make_unique<Label>();
        axis->AddClass("inspector-model-axis-name");
        axis->AddClass(spec.Class);
        axis->SetText(spec.Axis);
        chip->AddChild(std::move(axis));

        auto direction = std::make_unique<Label>();
        direction->AddClass("inspector-model-axis-direction");
        direction->SetText(spec.Direction);
        chip->AddChild(std::move(direction));

        legend->AddChild(std::move(chip));
    }
    root->AddChild(std::move(legend));
}

void BuildModelTab(UIElement* root,
                   const std::filesystem::path& assetPath,
                   std::function<void()> requestInspectorRefresh,
                   IThumbnailProvider* thumbs)
{
    InspectorUI::AddTextBlock(root, assetPath.string(), "inspector-asset-path");
    BuildOrientationPreview(root, assetPath, thumbs);
    const ModelFormat format = ModelAsset::FormatFromPath(assetPath);
    if (ModelAsset::UsesAxisImportOptions(format))
    {
        const FbxLoaderOptions fbxOpts = ResolveFbxLoaderOptions(assetPath);
        BuildBakeRotationSection(root, assetPath, fbxOpts, requestInspectorRefresh);
        BuildMirrorSection(root, assetPath, fbxOpts, requestInspectorRefresh);
    }
    else
    {
        AddHintBox(root, "This format has no bake or import flags. Use the LOD and Rig tabs.");
    }
    if (format == ModelFormat::FBX)
        BuildImportSettingsSection(root, assetPath, requestInspectorRefresh);
    else if (ModelAsset::UsesAxisImportOptions(format))
    {
        AddButton(root, "Reimport", [assetPath, requestInspectorRefresh] {
            ReloadModel(assetPath);
            if (requestInspectorRefresh)
                requestInspectorRefresh();
        }, "Force a reload from the source file. Bake and mirror edits already reload "
           "automatically; use this after changing the file on disk.");
    }
}

void BuildRigTab(UIElement* root,
                 ModelAsset* model,
                 const std::filesystem::path& assetPath,
                 std::function<void()> requestInspectorRefresh)
{
    InspectorUI::AddTextBlock(root, "Retargeting", "inspector-section-subheader");
    if (!ModelHasSkinnedMesh(model))
    {
        AddHintBox(root,
                   "This model has no skinned meshes. Rig kind is used for humanoid retargeting.");
        return;
    }

    AddHintBox(root,
               "Auto writes a humanoid sidecar when bone names pass coverage and "
               "rest pose is biped. Humanoid runs the same import. Not humanoid "
               "never writes one and removes leftover auto sidecars.");

    constexpr EnumEntry<ModelRigKind> kRigKinds[] = {
        {ModelRigKind::Auto,        "Auto"},
        {ModelRigKind::Humanoid,    "Humanoid (retarget)"},
        {ModelRigKind::NotHumanoid, "Not humanoid"},
    };
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
    const ModelAssetSettings rigSettings = ModelAssetSettings::Load(registry, assetPath);
    auto* rigField = InspectorUI::AddEnumRow(
        root, "Kind", kRigKinds, rigSettings.RigKind,
        "Controls whether this skinned model is treated as a humanoid for retargeting.");
    rigField->SetOnValueChanged(
        [assetPath, requestInspectorRefresh](ModelRigKind kind) {
            auto& assets = EngineCore::GetInstance().GetAssetManager();
            ModelAssetSettings edited = ModelAssetSettings::Load(assets.GetRegistry(), assetPath);
            edited.RigKind = kind;
            edited.Save(assets.GetRegistry(), assetPath);
            ReloadModel(assetPath);
            if (requestInspectorRefresh)
                requestInspectorRefresh();
        });
}

void BuildModelInspector(UIElement* root,
                         ModelAsset* model,
                         std::function<void()> requestInspectorRefresh,
                         IThumbnailProvider* thumbs)
{
    if (!root || !model)
        return;

    const std::filesystem::path assetPath = model->GetPath();
    const ModelInspectorTab tab = SelectedModelInspectorTab(assetPath);
    AddSubtabBar(root, assetPath, tab, requestInspectorRefresh);

    switch (tab)
    {
    case ModelInspectorTab::Lod:
        BuildLodSection(root, model, requestInspectorRefresh);
        break;
    case ModelInspectorTab::Rig:
        BuildRigTab(root, model, assetPath, requestInspectorRefresh);
        break;
    case ModelInspectorTab::Model:
    default:
        BuildModelTab(root, assetPath, requestInspectorRefresh, thumbs);
        break;
    }
}

} // namespace

void RegisterModelInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::Model,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;
            auto* asset = static_cast<Asset*>(ctx.Object);
            auto* model = dynamic_cast<ModelAsset*>(asset);
            if (!model)
                return;
            BuildModelInspector(ctx.Parent, model, ctx.RequestInspectorRefresh, ctx.Thumbnails);
        });
}

} // namespace GameEngine
