#include "Inspectors/MaterialInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/DeclaredPropertyRowModel.h"
#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Platform/SystemMetrics.h"
#include "UI/InspectorSection.h"
#include "Input/InputSystem.h"

#include "Assets/MaterialAsset.h"
#include "Assets/MaterialXImport.h"
#include "Assets/ShaderProgramAsset.h"
#include "Logger/Logger.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/RenderServices.h"
#include "Platform/Shell.h"
#include "UndoRedo/UndoRedoService.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"

#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/Materials/MaterialValidation.h"
#include "Rendering/Materials/ShaderCapabilityDetector.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Rendering/Materials/ShaderMetaValidation.h"
#include "Rendering/ShaderGraph/SgPropertyBinding.h"

#include "Editor/EditorPaths.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "Editor/Materials/EmissiveMapAssignment.h"
#include "Editor/Materials/MaterialRows.h"
#include "Editor/Materials/MaterialRuntimeTextures.h"
#include "Editor/Materials/MaterialUndoSnapshot.h"
#include "Panels/ShaderErrorRows.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/Vector3Field.h"
#include "UI/Controls/Foldout.h"
#include "UI/MaterialTexturePreviewHost.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/MaterialInspectorSections.h"
#include "Inspectors/UndeclaredKeysNotice.h"
#include "Panels/ColorPicker.h"
#include "UI/StyleProperties.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "Types/NearestName.h"
#include "Types/StringUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine
{

namespace
{

// ============================================================================
// Utility
// ============================================================================

// Extract base color from material document and convert to icon tint color
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

// JSON may store whole numbers as int32_t in MaterialValue; sliders need a float.
static bool TryGetScalarFromMaterialValue(const MaterialValue& val, float& out)
{
    if (const float* f = std::get_if<float>(&val))
    {
        out = *f;
        return true;
    }
    if (const int32_t* i = std::get_if<int32_t>(&val))
    {
        out = static_cast<float>(*i);
        return true;
    }
    return false;
}

static bool TryParseFloatList(const std::string& text, std::vector<float>& out)
{
    out.clear();
    std::string s = TrimWhitespace(text);
    if (!s.empty() && s.front() == '[' && s.back() == ']')
    {
        s = TrimWhitespace(s.substr(1, s.size() - 2));
    }

    std::istringstream stream(s);
    float value;
    while (stream >> value)
    {
        out.push_back(value);
        char sep;
        if (stream >> sep)
        {
            if (sep != ',' && sep != ' ')
            {
                out.clear();
                return false;
            }
        }
    }
    return !out.empty();
}

static std::string FormatFloatList(const std::vector<float>& vals)
{
    std::ostringstream oss;
    for (size_t i = 0; i < vals.size(); ++i)
    {
        if (i > 0)
            oss << ", ";
        oss << vals[i];
    }
    return oss.str();
}

// Convert float RGB/RGBA [0..1] to packed ARGB uint32
static uint32_t FloatColorToArgb(const std::vector<float>& c)
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    uint8_t r = (uint8_t)(clamp01(c.size() > 0 ? c[0] : 0.0f) * 255.0f + 0.5f);
    uint8_t g = (uint8_t)(clamp01(c.size() > 1 ? c[1] : 0.0f) * 255.0f + 0.5f);
    uint8_t b = (uint8_t)(clamp01(c.size() > 2 ? c[2] : 0.0f) * 255.0f + 0.5f);
    uint8_t a = (uint8_t)(clamp01(c.size() > 3 ? c[3] : 1.0f) * 255.0f + 0.5f);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Convert packed ARGB to float vector (3 or 4 components)
static std::vector<float> ArgbToFloatColor(uint32_t argb, size_t numComponents)
{
    auto toFloat = [](uint8_t c) { return c / 255.0f; };
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;
    if (numComponents == 4)
    {
        uint8_t a = (argb >> 24) & 0xFF;
        return {toFloat(r), toFloat(g), toFloat(b), toFloat(a)};
    }
    return {toFloat(r), toFloat(g), toFloat(b)};
}

// Helper to update the inspector header icon tint based on material base color
static void UpdateMaterialIconTint(UIElement* root, MaterialAsset* mat)
{
    if (!root || !mat)
        return;
    
    // Traverse up to find the parent InspectorSection
    UIElement* parent = root->GetParent();
    while (parent)
    {
        if (auto* section = dynamic_cast<InspectorSection*>(parent))
        {
            const MaterialDocument& doc = mat->GetDocument();
            const uint32_t tint = BaseColorToIconTint(doc);
            if (tint != 0u)
                section->SetHeaderIconTint(tint);
            break;
        }
        parent = parent->GetParent();
    }
}

// Canonical lighting model names
static constexpr const char* kLightingModelStandardPBR = "StandardPBR";
static constexpr const char* kLightingModelUnlit = "Unlit";
static constexpr const char* kLightingModelShadowOnly = "ShadowOnly";

// ============================================================================
// Lighting model
// ============================================================================

// Index into the Lighting Model dropdown's item list, which this file also
// builds — the pairing is why the mapping stays here rather than in the row
// model.
static int LightingModelDropdownIndex(const std::string& lightingModel)
{
    if (Editor::MaterialRows::IsLightingModelUnlit(lightingModel))
        return 1;
    if (Editor::MaterialRows::IsLightingModelShadowOnly(lightingModel))
        return 2;
    return 0;
}

// ============================================================================
// Serialization (v3)
// ============================================================================

static void SaveDocToDisk(MaterialAsset* asset, const MaterialDocument& doc)
{
    if (!asset)
        return;

    bool wrote = false;
    try
    {
        const std::filesystem::path path = asset->GetPath();
        // Imported (MaterialX) materials round-trip their supported properties back into the .mtlx
        // in its own format — never overwrite a .mtlx source with engine JSON.
        if (doc.shaderLocked && ToLowerAscii(path.extension().string()) == ".mtlx")
        {
            wrote = WriteMaterialX(path.string(), doc);
        }
        else
        {
            // Path-aware save: back-fills [path,guid] companions so the material
            // survives the asset-identity flip.
            std::ofstream out(path, std::ios::binary);
            if (!out.is_open())
            {
                return;
            }
            out << GameEngine::Editor::SerializeMaterialDocumentForSave(doc);
            wrote = true;
        }
    }
    catch (...)
    {
    }

    // Wake the VCS poll so the panel reflects the edit. The poll thread's
    // cooldown absorbs slider-drag bursts, so a flurry of calls collapses
    // into at most two polls (leading + trailing).
    if (wrote)
    {
        if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
            vcs->RefreshStatus();
    }
}

// ============================================================================
// Inspector rebuild / save
// ============================================================================

static void ClearChildren(UIElement* root)
{
    if (!root)
        return;
    root->RemoveAllChildren();
}

using PingAssetFn = std::function<void(const std::filesystem::path&)>;

static void RequestRebuild(UIElement* root, MaterialAsset* mat, OpenColorPickerWindowFn openPicker = {},
                           PingAssetFn pingAsset = {},
                           Editor::UndoRedoService* undo = nullptr,
                           bool inlineEmbed = false,
                           OpenAssetFn openAsset = {},
                           PingAssetFn pingAssetPreserveInspector = {})
{
    if (!root || !mat)
        return;
    auto action = [root,
                   mat,
                   openPicker = std::move(openPicker),
                   pingAsset = std::move(pingAsset),
                   undo,
                   inlineEmbed,
                   openAsset = std::move(openAsset),
                   pingAssetPreserveInspector = std::move(pingAssetPreserveInspector)]()
    {
        ClearChildren(root);
        BuildMaterialInspectorUI(root, mat, openPicker, pingAsset, undo, inlineEmbed, openAsset,
                                 pingAssetPreserveInspector);
    };
    // Always defer. Running ClearChildren synchronously here would destroy the
    // widget whose callback initiated the rebuild (e.g. an AssetField during a
    // drop) while its stack frame is still live, producing a use-after-free.
    // Drop handling for cross-window drags runs outside the normal UI event
    // dispatch window, so an IsInEventDispatch() check is not sufficient.
    root->PostAction(std::move(action));
}

// Save doc to disk, reload asset, and invalidate compile cache.
static void SaveAndInvalidate(MaterialAsset* mat, const MaterialDocument& doc)
{
    SaveDocToDisk(mat, doc);
    if (!mat)
        return;
    (void)mat->Reload();
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        rs->Materials().Compiler().Clear(mat->GetGUID());
        rs->RegisterAndPrewarmMaterial(mat->GetGUID(), doc);
    }
}

// Rebind all texture slots on the runtime Material from the asset document so
// GPU handles and bindless indices match disk (RegisterMaterialFromDocument
// binds non-empty paths, but UpdateMaterialTextures also clears slots and
// reapplies sampler state from the document), with every slot of the
// material's surface present so a removed binding clears
// (MaterialRuntimeTextures::WithEverySurfaceSlot).
static void PushRuntimeMaterialTextureBindings(MaterialAsset* mat)
{
    if (!mat)
        return;
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        const Engine::Renderer::Material* runtimeMaterial = rs->Materials().Registry().Find(mat->GetGUID());
        if (!runtimeMaterial)
            return;
        rs->Textures().UpdateMaterialTextures(
            mat->GetGUID(), Editor::MaterialRuntimeTextures::WithEverySurfaceSlot(mat->GetDocument(), *runtimeMaterial));
    }
}

static void SaveReloadRebuild(UIElement* root, MaterialAsset* mat, const MaterialDocument& doc,
                              OpenColorPickerWindowFn openPicker = {},
                              PingAssetFn pingAsset = {},
                              Editor::UndoRedoService* undo = nullptr,
                              bool inlineEmbed = false,
                              OpenAssetFn openAsset = {},
                              PingAssetFn pingAssetPreserveInspector = {})
{
    SaveAndInvalidate(mat, doc);
    RequestRebuild(root, mat, std::move(openPicker), std::move(pingAsset), undo, inlineEmbed,
                   std::move(openAsset), std::move(pingAssetPreserveInspector));
}

// Lightweight live-preview update for continuous drags (slider, color picker).
// Pushes the value directly to the runtime Material's CPU cache for instant
// viewport feedback (repacked into the shared MaterialParams SSBO next frame)
// without writing to disk, re-parsing JSON, or clearing compile cache.
static void UpdateRuntimeProperty(MaterialAsset* mat, const std::string& propKey,
                                   const MaterialValue& value)
{
    if (!mat)
        return;
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;
    auto* rtMat = rs->Materials().Registry().Find(mat->GetGUID());
    if (!rtMat)
        return;

    const StringId sid = HashStringId(propKey);
    if (const float* fval = std::get_if<float>(&value))
    {
        rtMat->SetFloat(sid, *fval);
        if (propKey == "opacity")
        {
            float rgba[4];
            if (rtMat->GetVector(HashStringId("baseColor"), rgba, 4))
            {
                rgba[3] = std::max(0.0f, *fval);
                rtMat->SetVector(HashStringId("baseColor"), rgba, 4);
            }
        }
    }
    else if (const std::vector<float>* vval = std::get_if<std::vector<float>>(&value))
    {
        if (!vval->empty())
            rtMat->SetVector(sid, vval->data(), static_cast<uint32_t>(vval->size()));
    }
    else if (const int32_t* ival = std::get_if<int32_t>(&value))
    {
        // Document integers land as float: every material param lane is a float
        // vec4, and raw int bits read as denormals (see MaterialRegistry.cpp).
        rtMat->SetFloat(sid, static_cast<float>(*ival));
    }
    // The setters above bump the global content epoch; PackMaterialSSBO repacks
    // the shared MaterialParams SSBO next frame, so no explicit upload is needed.
}

// Push a live texture transform update directly to the runtime Material.
// MaterialAsset::Reload() re-parses the JSON doc but does NOT re-apply
// textureTransforms to the registry-owned runtime Material, so without this
// direct push, tiling/offset edits persist to disk yet never show up in the
// viewport until the editor restarts.
static void UpdateRuntimeTextureTransform(MaterialAsset* mat,
                                          const std::string& texName,
                                          const std::array<float, 8>& st)
{
    if (!mat)
        return;
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;
    auto* rtMat = rs->Materials().Registry().Find(mat->GetGUID());
    if (!rtMat)
        return;

    // Routed like the bindings: a surface's own texture name carries its tiling on the ordinal it
    // binds to, and a name the surface does not have is a no-op.
    rtMat->SetTextureTransform(HashStringId(texName), st);
}

// Re-push every texture transform on the MaterialDocument to the runtime
// Material. Used by undo/redo when the document is restored from a snapshot
// so the viewport reflects the reverted tiling/offset state.
static void PushAllTextureTransformsToRuntime(MaterialAsset* mat)
{
    if (!mat)
        return;
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;
    auto* rtMat = rs->Materials().Registry().Find(mat->GetGUID());
    if (!rtMat)
        return;

    Editor::MaterialRuntimeTextures::ApplyTextureTransforms(mat->GetDocument(), *rtMat);
}

// Build a SnapshotTarget for undo/redo over a MaterialAsset's JSON on disk.
// Capture: serializes the current document to JSON bytes.
// Apply:   writes bytes back to disk, reloads the asset, then re-pushes
//          texture transforms live to the runtime Material so the viewport
//          reflects the reverted state immediately.
static Editor::UndoRedoService::SnapshotTarget MakeMaterialSnapshotTarget(MaterialAsset* mat,
                                                                           const std::string& label)
{
    Editor::UndoRedoService::SnapshotTarget target;
    target.debugLabel = label;

    // The file as it is on disk, not the loaded document: the document carries the defaults
    // filled in at load, which undo would otherwise write into the author's file.
    target.Capture = [mat](Editor::UndoRedoService::SnapshotTarget::Snapshot& outSnapshot) -> bool
    {
        return mat && Editor::MaterialRows::ReadMaterialFileSnapshot(mat->GetPath(), outSnapshot);
    };

    target.Apply = [mat](const Editor::UndoRedoService::SnapshotTarget::Snapshot& snapshot) -> bool
    {
        if (!mat)
            return false;
        try
        {
            std::ofstream out(mat->GetPath(), std::ios::binary);
            if (!out.is_open())
                return false;
            if (!snapshot.empty())
                out.write(reinterpret_cast<const char*>(snapshot.data()),
                          static_cast<std::streamsize>(snapshot.size()));
        }
        catch (...)
        {
            return false;
        }

        (void)mat->Reload();
        PushAllTextureTransformsToRuntime(mat);
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            rs->Materials().Compiler().Clear(mat->GetGUID());
            rs->RegisterAndPrewarmMaterial(mat->GetGUID(), mat->GetDocument());
        }
        PushRuntimeMaterialTextureBindings(mat);
        if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
            vcs->RefreshStatus();
        return true;
    };

    return target;
}

// Default seeding lives on MaterialDocument (FillMissingStandardPBRDefaults) so the
// parse path and this one cannot drift; call it directly.

// ============================================================================
// Color swatch helper
// ============================================================================

static std::string FormatColorRgba(const std::vector<float>& c)
{
    char buf[64];
    if (c.size() >= 4)
        std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f, %.2f)", c[0], c[1], c[2], c[3]);
    else if (c.size() >= 3)
        std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", c[0], c[1], c[2]);
    else
        std::snprintf(buf, sizeof(buf), "(...)");
    return buf;
}

// An HDR colour is shown and edited as a normalised chromaticity times an
// intensity: the picker's intensity channel carries max(1, brightest component).
static float HdrIntensity(const std::vector<float>& color)
{
    float peak = 1.0f;
    for (size_t i = 0; i < color.size() && i < 3; ++i)
        peak = std::max(peak, color[i]);
    return peak;
}

static std::vector<float> HdrNormalized(const std::vector<float>& color)
{
    const float intensity = HdrIntensity(color);
    std::vector<float> out = color;
    for (size_t i = 0; i < out.size() && i < 3; ++i)
        out[i] /= intensity;
    return out;
}

static std::vector<float> HdrCompose(const std::vector<float>& normalized, float intensity)
{
    std::vector<float> out = normalized;
    for (size_t i = 0; i < out.size() && i < 3; ++i)
        out[i] *= intensity;
    return out;
}

static void AddColorSwatchRow(UIElement* parent, const std::string& labelText,
                               const std::vector<float>& currentColor,
                               UIElement* root, MaterialAsset* mat, const std::string& propKey,
                               OpenColorPickerWindowFn openPicker, bool inlineEmbed,
                               OpenAssetFn openAsset = {}, bool hdr = false,
                               const char* tooltip = nullptr)
{
    using namespace InspectorDrag;
    UIElement* row = InspectorUI::AddRow(parent);
    Label* lbl = InspectorUI::AddLabel(row, labelText, tooltip);
    if (lbl) lbl->AddClass("inspector-label-no-drag");
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    const uint32_t argb = FloatColorToArgb(hdr ? HdrNormalized(currentColor) : currentColor);
    const size_t numComponents = currentColor.size();

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    InspectorUI::StyleColorSwatch(swatchRaw, argb);
    fieldContainer->AddChild(std::move(swatch));

    auto rgbaLabel = std::make_unique<Label>();
    rgbaLabel->AddClass("inspector-text");
    rgbaLabel->SetText(FormatColorRgba(currentColor));
    rgbaLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    Label* rgbaLabelRaw = rgbaLabel.get();
    fieldContainer->AddChild(std::move(rgbaLabel));

    // The color picker is an app-owned singleton whose live-preview callbacks outlive
    // this row: a material rebuild frees the swatch/label while the picker stays open.
    const UIElement::WeakRef<> swatchRef = UIElement::MakeWeakRef(swatchRaw);
    const UIElement::WeakRef<Label> rgbaLabelRef = UIElement::MakeWeakRef(rgbaLabelRaw);
    auto updateUI = [swatchRef, rgbaLabelRef, hdr](const std::vector<float>& color) {
        UIElement* swatch = swatchRef.Get();
        Label* rgbaLabel = rgbaLabelRef.Get();
        if (!swatch || !rgbaLabel)
            return;
        InspectorUI::StyleColorSwatch(swatch, FloatColorToArgb(hdr ? HdrNormalized(color) : color));
        rgbaLabel->SetText(FormatColorRgba(color));
    };

    auto clickHandler = [root, mat, numComponents, propKey, openPicker, inlineEmbed, openAsset, hdr, currentColor, updateUI](UIEvent& e) {
        if (e.Button != 0)
            return;
        e.Stop();

        if (!openPicker)
            return;

        std::vector<float> originalColor = currentColor;
        {
            const MaterialDocument& d = mat->GetDocument();
            auto it = d.properties.find(propKey);
            if (it != d.properties.end() && std::holds_alternative<std::vector<float>>(it->second))
                originalColor = std::get<std::vector<float>>(it->second);
        }

        const float originalIntensity = hdr ? HdrIntensity(originalColor) : 1.0f;
        uint32_t currentArgb = FloatColorToArgb(hdr ? HdrNormalized(originalColor) : originalColor);
        ColorPickerCallbacks cbs;
        cbs.onApply = [root, mat, numComponents, propKey, openPicker, inlineEmbed, openAsset, hdr](uint32_t newArgb, float intensity) {
            std::vector<float> newColor = ArgbToFloatColor(newArgb, numComponents);
            if (hdr)
                newColor = HdrCompose(newColor, intensity);
            MaterialDocument d = mat->GetDocument();
            // baseColor.a and `opacity` are two spellings of one quantity, and
            // `opacity` is what survives to the GPU. Carry an alpha edit across to it
            // when the two currently agree; a document that deliberately authored them
            // apart keeps its own value (the Opacity row is always shown, so that case
            // stays editable).
            const bool carryAlphaToOpacity = [&] {
                if (propKey != "baseColor")
                    return false;
                auto it = d.properties.find("opacity");
                if (it == d.properties.end())
                    return false;
                const float* stored = std::get_if<float>(&it->second);
                return stored && std::fabs(*stored - d.DefaultOpacity()) <= 1e-6f;
            }();
            d.properties[propKey] = newColor;
            if (carryAlphaToOpacity)
                d.properties["opacity"] = d.DefaultOpacity();
            SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
            UpdateMaterialIconTint(root, mat);
        };
        cbs.onCancel = [mat, propKey, originalColor, updateUI]() {
            UpdateRuntimeProperty(mat, propKey, MaterialValue{originalColor});
            updateUI(originalColor);
        };
        cbs.onValueChanging = [mat, numComponents, propKey, hdr, updateUI](uint32_t newArgb, float intensity) {
            std::vector<float> newColor = ArgbToFloatColor(newArgb, numComponents);
            if (hdr)
                newColor = HdrCompose(newColor, intensity);
            UpdateRuntimeProperty(mat, propKey, MaterialValue{newColor});
            updateUI(newColor);
        };
        openPicker(currentArgb, originalIntensity, std::move(cbs));
    };

    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    rgbaLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
}

static bool TryReadFloatProperty(const MaterialDocument& doc, const std::string& key, float& out)
{
    auto it = doc.properties.find(key);
    if (it == doc.properties.end())
        return false;
    if (const float* f = std::get_if<float>(&it->second))
    {
        out = *f;
        return true;
    }
    if (const int32_t* i = std::get_if<int32_t>(&it->second))
    {
        out = static_cast<float>(*i);
        return true;
    }
    return false;
}

static std::vector<float> ReadShaderGraphColorFromKeys(const MaterialDocument& doc,
                                                       const std::vector<std::string>& keys)
{
    std::vector<float> color = {0.0f, 0.0f, 0.0f, 1.0f};
    if (keys.size() >= 3)
    {
        TryReadFloatProperty(doc, keys[0], color[0]);
        TryReadFloatProperty(doc, keys[1], color[1]);
        TryReadFloatProperty(doc, keys[2], color[2]);
    }
    return color;
}

static void PushShaderGraphColorToRuntime(MaterialAsset* mat, const std::vector<std::string>& keys,
                                          const std::vector<float>& color)
{
    if (keys.size() < 3 || color.size() < 3)
        return;
    UpdateRuntimeProperty(mat, keys[0], MaterialValue{color[0]});
    UpdateRuntimeProperty(mat, keys[1], MaterialValue{color[1]});
    UpdateRuntimeProperty(mat, keys[2], MaterialValue{color[2]});
}

static void WriteShaderGraphColorToDocument(MaterialDocument& doc, const std::vector<std::string>& keys,
                                            const std::vector<float>& color)
{
    if (keys.size() < 3 || color.size() < 3)
        return;
    doc.properties[keys[0]] = color[0];
    doc.properties[keys[1]] = color[1];
    doc.properties[keys[2]] = color[2];
}

static void AddShaderGraphAliasedColorRow(UIElement* parent, const std::string& labelText,
                                          const std::vector<std::string>& materialKeys,
                                          UIElement* root, MaterialAsset* mat,
                                          OpenColorPickerWindowFn openPicker, bool inlineEmbed,
                                          OpenAssetFn openAsset = {})
{
    using namespace InspectorDrag;
    const MaterialDocument& doc = mat->GetDocument();
    const std::vector<float> currentColor = ReadShaderGraphColorFromKeys(doc, materialKeys);

    UIElement* row = InspectorUI::AddRow(parent);
    Label* lbl = InspectorUI::AddLabel(row, labelText);
    if (lbl)
        lbl->AddClass("inspector-label-no-drag");
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    const uint32_t argb = FloatColorToArgb(currentColor);

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    InspectorUI::StyleColorSwatch(swatchRaw, argb);
    fieldContainer->AddChild(std::move(swatch));

    auto rgbaLabel = std::make_unique<Label>();
    rgbaLabel->AddClass("inspector-text");
    rgbaLabel->SetText(FormatColorRgba(currentColor));
    rgbaLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    Label* rgbaLabelRaw = rgbaLabel.get();
    fieldContainer->AddChild(std::move(rgbaLabel));

    const UIElement::WeakRef<> swatchRef = UIElement::MakeWeakRef(swatchRaw);
    const UIElement::WeakRef<Label> rgbaLabelRef = UIElement::MakeWeakRef(rgbaLabelRaw);
    auto updateUI = [swatchRef, rgbaLabelRef](const std::vector<float>& color) {
        UIElement* swatch = swatchRef.Get();
        Label* rgbaLabel = rgbaLabelRef.Get();
        if (!swatch || !rgbaLabel)
            return;
        InspectorUI::StyleColorSwatch(swatch, FloatColorToArgb(color));
        rgbaLabel->SetText(FormatColorRgba(color));
    };

    auto clickHandler = [root, mat, materialKeys, openPicker, inlineEmbed, openAsset, updateUI](UIEvent& e) {
        if (e.Button != 0)
            return;
        e.Stop();
        if (!openPicker)
            return;

        const std::vector<float> originalColor =
            ReadShaderGraphColorFromKeys(mat->GetDocument(), materialKeys);

        ColorPickerCallbacks cbs;
        cbs.onApply = [root, mat, materialKeys, openPicker, inlineEmbed, openAsset](uint32_t newArgb, float) {
            std::vector<float> newColor = ArgbToFloatColor(newArgb, 3);
            MaterialDocument d = mat->GetDocument();
            WriteShaderGraphColorToDocument(d, materialKeys, newColor);
            SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
        };
        cbs.onCancel = [mat, materialKeys, originalColor, updateUI]() {
            PushShaderGraphColorToRuntime(mat, materialKeys, originalColor);
            updateUI(originalColor);
        };
        cbs.onValueChanging = [mat, materialKeys, updateUI](uint32_t newArgb, float) {
            std::vector<float> newColor = ArgbToFloatColor(newArgb, 3);
            PushShaderGraphColorToRuntime(mat, materialKeys, newColor);
            updateUI(newColor);
        };
        openPicker(FloatColorToArgb(originalColor), 1.0f, std::move(cbs));
    };

    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    rgbaLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
}

static std::unordered_set<std::string> CollectShaderGraphAliasedPropertyKeys(
    const MaterialDocument& doc)
{
    std::unordered_set<std::string> keys;
    for (const MaterialShaderGraphPublicProperty& graphProp : doc.shaderGraphPublicProperties)
    {
        for (const std::string& key : graphProp.MaterialPropertyKeys)
            keys.insert(key);
    }
    return keys;
}

// ============================================================================
// Slider row helper (0..1)
// ============================================================================

static InspectorDrag::SliderWithFloatValueRow AddMaterialSliderRow(
    UIElement* parent, const std::string& labelText, float initial, float min, float max, UIElement* root,
    MaterialAsset* mat, const std::string& propKey, OpenColorPickerWindowFn openPicker,
    Editor::UndoRedoService* undo, bool inlineEmbed, OpenAssetFn openAsset = {}, const char* tooltip = nullptr)
{
    using namespace InspectorDrag;
    auto sliderRow = AddSliderWithFloatValueRow(parent, labelText, initial, min, max, tooltip);
    Slider* sliderRaw = sliderRow.Slider;
    FloatField* valueField = sliderRow.ValueField;
    if (!sliderRaw || !valueField)
        return sliderRow;

    using OptEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
    auto activeEdit = std::make_shared<OptEdit>();
    auto beginEditIfNeeded = [undo, mat, activeEdit, propKey]() {
        if (!undo || !mat || !activeEdit || activeEdit->has_value())
            return;
        activeEdit->emplace(
            undo->BeginInteractiveEdit(std::string("Material ") + propKey, MakeMaterialSnapshotTarget(mat, propKey)));
    };

    // One clamp for both controls; each edit mirrors into its twin without
    // re-notifying, so a slider drag updates the typed value and vice versa.
    auto sync = [sliderRaw, valueField, min, max](float rawValue) {
        const float v = std::clamp(rawValue, min, max);
        sliderRaw->SetValueWithoutNotify(v);
        valueField->SetValueWithoutNotify(v);
        return v;
    };

    auto preview = [mat, propKey, beginEditIfNeeded, sync](const float& rawValue) {
        beginEditIfNeeded();
        UpdateRuntimeProperty(mat, propKey, sync(rawValue));
    };

    auto commit = [root, mat, propKey, openPicker, inlineEmbed, openAsset, beginEditIfNeeded,
                   activeEdit, sync](const float& rawValue) {
        beginEditIfNeeded();
        const float v = sync(rawValue);
        MaterialDocument d = mat->GetDocument();
        d.properties[propKey] = v;
        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
        if (activeEdit && activeEdit->has_value())
        {
            if (activeEdit->value())
                activeEdit->value().Commit();
            activeEdit->reset();
        }
    };

    sliderRaw->SetOnValueChanging(preview);
    sliderRaw->SetOnValueChanged(commit);
    valueField->SetOnValueChanging(preview);
    valueField->SetOnValueChanged(commit);

    SetupLabelDragSlider(sliderRow.Label, sliderRaw, nullptr, nullptr, initial);

    return sliderRow;
}

static Toggle* AddMaterialToggleRow(UIElement* parent, const std::string& labelText, bool initial,
                                    UIElement* root, MaterialAsset* mat, const std::string& propKey,
                                    OpenColorPickerWindowFn openPicker, Editor::UndoRedoService* undo,
                                    bool inlineEmbed, OpenAssetFn openAsset = {},
                                    const char* tooltip = nullptr)
{
    return InspectorDrag::AddToggleRow(parent, labelText, initial,
        [root, mat, propKey, openPicker, undo, inlineEmbed, openAsset](bool v)
        {
            MaterialDocument d = mat->GetDocument();
            if (propKey == "doubleSided")
                d.doubleSided = v;
            else
                d.properties[propKey] = v;
            if (propKey == "hexTiling" && v)
            {
                if (d.properties.find("hexBlend") == d.properties.end())
                    d.properties["hexBlend"] = 0.5f;
                if (d.properties.find("hexRotation") == d.properties.end())
                    d.properties["hexRotation"] = 1.0f;
            }

            if (undo)
            {
                Editor::UndoRedoService::InteractiveEdit edit =
                    undo->BeginInteractiveEdit(std::string("Material ") + propKey,
                                               MakeMaterialSnapshotTarget(mat, propKey));
                if (edit)
                {
                    SaveAndInvalidate(mat, d);
                    edit.Commit();
                    RequestRebuild(root, mat, openPicker, {}, undo, inlineEmbed, openAsset);
                    return;
                }
            }
            SaveReloadRebuild(root, mat, d, openPicker, {}, undo, inlineEmbed, openAsset);
        }, tooltip);
}

// How the document's surface takes its textures, decided from the document as it stands by the rule
// registration applies (MaterialSystem::ResolveSurfaceTextureUse), so a material no open scene uses
// lays out like a placed one and an edit shows its effect at the next rebuild. Empty without render
// services.
static Engine::Renderer::MaterialSystem::SurfaceTextureUse ResolveSurfaceTextureUse(MaterialAsset* mat,
                                                                                   const MaterialDocument& doc)
{
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    return rs ? rs->Materials().ResolveSurfaceTextureUse(doc, mat->GetPath())
              : Engine::Renderer::MaterialSystem::SurfaceTextureUse{};
}

// The rows that belong to a bound height map, under its texture block: Relief Depth where the relief
// march reads it, and why the march is refused where it is (hex tiling, or a surface without a height
// slot), in the words that state the fix.
static void AddHeightMapRows(UIElement* content, const MaterialDocument& doc,
                             const Engine::Renderer::MaterialSystem::SurfaceTextureUse& surfaceTextures,
                             UIElement* root, MaterialAsset* mat, OpenColorPickerWindowFn openPicker,
                             Editor::UndoRedoService* undo, bool inlineEmbed, OpenAssetFn openAsset)
{
    const std::string& refusal = surfaceTextures.ParallaxRefusal;
    if (Editor::MaterialRows::IsReliefDepthVisible(doc, surfaceTextures.DeclaredNames))
    {
        // The engine's default when the document authors none; the row writes nothing until moved.
        float reliefDepth = kDefaultReliefDepth;
        if (auto it = doc.properties.find("reliefDepth"); it != doc.properties.end())
            (void)TryGetScalarFromMaterialValue(it->second, reliefDepth);
        const auto row = AddMaterialSliderRow(content, "Relief Depth", reliefDepth, 0.0f,
                                              Editor::MaterialRows::kReliefDepthSliderMax, root, mat, "reliefDepth",
                                              openPicker, undo, inlineEmbed, openAsset,
                                              Editor::MaterialRows::kReliefDepthTooltip);
        // While the relief is refused the depth does nothing, until the fix the warning states.
        if (!refusal.empty())
            Editor::MakeMaterialRowInactive(row);
    }

    if (!refusal.empty())
        Editor::AddMaterialWarning(content, refusal);
}

// ============================================================================
// Declared properties: rows generated from the surface's `// @property` table
// ============================================================================

// N float fields on one row for vec2/vec3/vec4 properties.
static void AddDeclaredVectorRow(UIElement* parent, const Rendering::ShaderProperty& p,
                                 const std::array<float, 4>& value, UIElement* root, MaterialAsset* mat,
                                 OpenColorPickerWindowFn openPicker, bool inlineEmbed,
                                 OpenAssetFn openAsset)
{
    using namespace InspectorDrag;
    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, p.DisplayName, p.Tooltip.empty() ? nullptr : p.Tooltip.c_str());
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(4.0f));

    const uint32_t n = p.ComponentCount();
    auto fields = std::make_shared<std::vector<FloatField*>>();
    auto collect = [fields, n]()
    {
        std::vector<float> out;
        out.reserve(n);
        for (FloatField* f : *fields)
            out.push_back(f->GetValue());
        return out;
    };
    const std::string key = p.Name;
    for (uint32_t i = 0; i < n; ++i)
    {
        FloatField* field = InspectorUI::AddFloat(fieldContainer, value[i]);
        fields->push_back(field);
        field->SetOnValueChanging([mat, key, collect](const float&) {
            UpdateRuntimeProperty(mat, key, MaterialValue{collect()});
        });
        field->SetOnValueChanged([root, mat, key, collect, openPicker, inlineEmbed, openAsset](const float&) {
            MaterialDocument d = mat->GetDocument();
            d.properties[key] = collect();
            SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
        });
    }
}

static void AddDeclaredPropertyRows(UIElement* content, const Rendering::ShaderPropertyTable& table,
                                    const MaterialDocument& doc, UIElement* root, MaterialAsset* mat,
                                    OpenColorPickerWindowFn openPicker, Editor::UndoRedoService* undo,
                                    bool inlineEmbed, OpenAssetFn openAsset)
{
    using namespace InspectorDrag;
    // Groups are collapsible sections created where their first member appears,
    // so the author's declaration order is the inspector order throughout.
    std::unordered_map<std::string, UIElement*> groups;
    for (const Rendering::ShaderProperty& p : table.Properties)
    {
        if (p.Hidden || !p.HasLane)
            continue; // hidden, or an adapter constant nothing stores
        if (!Editor::EvaluateVisibleIf(p.VisibleIf, table, doc))
            continue;

        UIElement* parent = content;
        if (!p.Group.empty())
        {
            auto it = groups.find(p.Group);
            if (it == groups.end())
            {
                std::unique_ptr<Foldout> foldout = Editor::MakeMaterialSection(p.Group, true);
                UIElement* groupContent = foldout->GetContentContainer();
                content->AddChild(std::move(foldout));
                it = groups.emplace(p.Group, groupContent).first;
            }
            parent = it->second;
        }

        std::array<float, 4> value{};
        Editor::ReadDeclaredValue(doc, p, value);
        const std::string key = p.Name;
        const char* tooltip = p.Tooltip.empty() ? nullptr : p.Tooltip.c_str();

        switch (p.Type)
        {
        case Rendering::ShaderPropertyType::Float:
            if (p.HasRange)
            {
                AddMaterialSliderRow(parent, p.DisplayName, std::clamp(value[0], p.RangeMin, p.RangeMax), p.RangeMin,
                                     p.RangeMax, root, mat, key, openPicker, undo, inlineEmbed, openAsset,
                                     tooltip);
            }
            else
            {
                AddFloatRowWithDrag(parent, p.DisplayName, value[0],
                    [mat, key](float v) { UpdateRuntimeProperty(mat, key, MaterialValue{v}); },
                    [root, mat, key, openPicker, inlineEmbed, openAsset](float v) {
                        MaterialDocument d = mat->GetDocument();
                        d.properties[key] = v;
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    },
                    p.Default[0], tooltip);
            }
            break;
        case Rendering::ShaderPropertyType::Int:
            AddIntRowWithDrag(parent, p.DisplayName, static_cast<int>(value[0]),
                [mat, key](int v) { UpdateRuntimeProperty(mat, key, MaterialValue{static_cast<int32_t>(v)}); },
                [root, mat, key, openPicker, inlineEmbed, openAsset](int v) {
                    MaterialDocument d = mat->GetDocument();
                    d.properties[key] = static_cast<int32_t>(v);
                    SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                },
                static_cast<int>(p.Default[0]), tooltip);
            break;
        case Rendering::ShaderPropertyType::Enum:
        {
            std::vector<Dropdown::Option> options;
            options.reserve(p.EnumValues.size());
            for (const std::string& label : p.EnumValues)
                options.push_back({label, label, {}});
            const int index = std::clamp(static_cast<int>(value[0]), 0,
                                         static_cast<int>(p.EnumValues.size()) - 1);
            Dropdown* dd = InspectorUI::AddDropdownRow(parent, p.DisplayName, options, index, tooltip);
            const std::vector<std::string> labels = p.EnumValues;
            dd->SetOnValueChanged([root, mat, key, labels, openPicker, inlineEmbed, openAsset](const std::string& v) {
                const auto it = std::find(labels.begin(), labels.end(), v);
                if (it == labels.end())
                    return;
                MaterialDocument d = mat->GetDocument();
                d.properties[key] = static_cast<int32_t>(it - labels.begin());
                SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
            });
            break;
        }
        case Rendering::ShaderPropertyType::Bool:
            AddMaterialToggleRow(parent, p.DisplayName, value[0] != 0.0f, root, mat, key, openPicker, undo,
                                 inlineEmbed, openAsset, tooltip);
            break;
        case Rendering::ShaderPropertyType::Color:
        {
            const std::vector<float> color(value.begin(), value.begin() + p.ComponentCount());
            AddColorSwatchRow(parent, p.DisplayName, color, root, mat, key, openPicker, inlineEmbed,
                              openAsset, p.Hdr, tooltip);
            break;
        }
        case Rendering::ShaderPropertyType::Vec2:
        case Rendering::ShaderPropertyType::Vec3:
        case Rendering::ShaderPropertyType::Vec4:
            AddDeclaredVectorRow(parent, p, value, root, mat, openPicker, inlineEmbed, openAsset);
            break;
        }
    }

    // Keys the document authors that reach no lane store nothing on the GPU;
    // say so — an unknown key with the nearest declared name, a key matching a
    // laneless declaration (an adapter constant) with the declare-it fix-it.
    // The StandardPBR parse-time fill still seeds the built-in key set into
    // every document, so those keys are not reported here until that fill goes
    // with the engine surfaces.
    std::vector<std::string> declaredNames;
    for (const Rendering::ShaderProperty& p : table.Properties)
        if (p.HasLane)
            declaredNames.push_back(p.Name);
    std::vector<std::string> unknown;
    for (const auto& [key, value] : doc.properties)
    {
        const Rendering::ShaderProperty* entry = table.Find(key);
        if (entry && entry->HasLane)
            continue;
        unknown.push_back(key);
    }
    Editor::AddUndeclaredKeysNotice(content, unknown, declaredNames, &table);
}

} // namespace

// ============================================================================
// BuildMaterialInspectorUI — the main inspector builder
// ============================================================================

static std::filesystem::path ResolveMaterialSurfaceShaderPath(const MaterialAsset* mat,
                                                              const MaterialDocument& doc)
{
    std::filesystem::path shaderPath;
    if (!doc.surfaceShaderGuid.empty())
    {
        AssetMetadata meta;
        auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        if (reg.TryGetAssetMetadata(GUID(doc.surfaceShaderGuid), meta))
            shaderPath = meta.Path;
    }
    if (shaderPath.empty() && !doc.surfaceShader.empty())
    {
        shaderPath = doc.surfaceShader;
        if (!shaderPath.is_absolute())
            shaderPath = (mat->GetPath().parent_path() / shaderPath).lexically_normal();
    }
    return shaderPath;
}

static std::filesystem::path ResolveMaterialSurfaceGraphPath(const MaterialAsset* mat,
                                                             const MaterialDocument& doc)
{
    std::filesystem::path graphPath;
    if (!doc.surfaceGraphGuid.empty())
    {
        AssetMetadata meta;
        auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
        if (reg.TryGetAssetMetadata(GUID(doc.surfaceGraphGuid), meta))
            graphPath = meta.Path;
    }
    if (graphPath.empty() && !doc.surfaceGraph.empty())
    {
        graphPath = doc.surfaceGraph;
        if (!graphPath.is_absolute())
            graphPath = (mat->GetPath().parent_path() / graphPath).lexically_normal();
    }
    return graphPath;
}

void BuildMaterialInspectorUI(UIElement* root, MaterialAsset* mat, OpenColorPickerWindowFn openPicker,
                              std::function<void(const std::filesystem::path&)> pingAsset,
                              Editor::UndoRedoService* undo,
                              bool inlineEmbed,
                              OpenAssetFn openAsset,
                              std::function<void(const std::filesystem::path&)> pingAssetPreserveInspector)
{
    using namespace InspectorDrag;
    if (!root || !mat)
        return;

    root->AddClass("material-inspector");

    const MaterialDocument& doc = mat->GetDocument();
    auto& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();

    // Async-compile on inspector open. Returns immediately; result lands in
    // the cache on a worker thread. The diagnostics section below renders a
    // "compiling..." state until then. Selection used to block the UI thread
    // for hundreds of ms via the synchronous CompileNow path.
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        rs->Materials().Compiler().RequestCompile(mat->GetGUID());
    }

    // Show parse errors (if any)
    if (!mat->GetErrors().empty())
    {
        std::ostringstream oss;
        oss << "Errors:\n";
        for (const auto& e : mat->GetErrors())
            oss << " - " << e << "\n";
        Editor::AddMaterialLine(root, oss.str());
    }

    // ========================================================================
    // [v] Shader section
    // ========================================================================
    {
        std::unique_ptr<Foldout> section = Editor::MakeMaterialSection("Shader", true);
        UIElement* content = section->GetContentContainer();

        // Lighting Model. Imported (MaterialX) materials are pinned to StandardPBR, so the model
        // and the surface-shader pickers (Advanced Shader section below) are shown read-only/hidden.
        if (doc.shaderLocked)
        {
            using namespace InspectorDrag;
            UIElement* lockedRow = InspectorUI::AddRow(content);
            InspectorUI::AddLabel(lockedRow, "Lighting Model");
            InspectorUI::AddLabel(lockedRow, doc.lightingModel + "  (MaterialX \xE2\x80\x94 shader locked)");

            // Escape hatch: bake the imported material into a native, fully-editable sibling
            // .material (shader no longer locked). The .mtlx remains as the read-only source.
            auto convertBtn = std::make_unique<Button>();
            convertBtn->AddClass("inspector-text");
            convertBtn->AddClass("material-line");
            convertBtn->SetText("Convert to .material (editable copy)");
            convertBtn->RegisterEventHandler(kEventButtonClick, [mat, pingAsset](UIEvent&) {
                MaterialDocument d = mat->GetDocument();
                d.shaderLocked = false;
                std::filesystem::path dst = mat->GetPath();
                dst.replace_extension(".material");
                bool wrote = false;
                try
                {
                    nlohmann::json j = SerializeMaterialDocument(d);
                    std::ofstream out(dst, std::ios::binary);
                    if (out.is_open())
                    {
                        out << j.dump(2);
                        wrote = true;
                    }
                }
                catch (...)
                {
                }
                if (!wrote)
                    Logger::Log::Warning("Convert to .material: failed to write '{}'", dst.string());
                else if (pingAsset)
                    pingAsset(dst); // only surface the new asset once it actually exists on disk
            });
            content->AddChild(std::move(convertBtn));

            root->AddChild(std::move(section));
        }
        else
        {
        const int lightingIdx = LightingModelDropdownIndex(doc.lightingModel);

        Dropdown* lightingDD = InspectorUI::AddDropdownRow(content, "Lighting Model",
            {{kLightingModelStandardPBR, kLightingModelStandardPBR},
             {kLightingModelUnlit, kLightingModelUnlit},
             {kLightingModelShadowOnly, "Shadow Only"}}, lightingIdx);

        lightingDD->SetOnValueChanged([root, mat, openPicker, undo, inlineEmbed, openAsset](const std::string& value) {
            MaterialDocument d = mat->GetDocument();
            d.lightingModel = value;
            if (Editor::MaterialRows::IsLightingModelShadowOnly(value))
            {
                d.alphaMode = MaterialAlphaMode::Blend;
                d.properties["baseColor"] = std::vector<float>{0.0f, 0.0f, 0.0f, 1.0f};
            }
            d.FillMissingStandardPBRDefaults();
            if (undo)
            {
                Editor::UndoRedoService::InteractiveEdit edit =
                    undo->BeginInteractiveEdit("Material lightingModel",
                                               MakeMaterialSnapshotTarget(mat, "lightingModel"));
                if (edit)
                {
                    SaveAndInvalidate(mat, d);
                    edit.Commit();
                    RequestRebuild(root, mat, openPicker, {}, undo, inlineEmbed, openAsset);
                    return;
                }
            }
            SaveReloadRebuild(root, mat, d, openPicker, {}, undo, inlineEmbed, openAsset);
        });

        root->AddChild(std::move(section));
        }

    }

    // ========================================================================
    // [v] Render State section
    // ========================================================================
    {
        std::unique_ptr<Foldout> section = Editor::MakeMaterialSection("Render State", true);
        UIElement* content = section->GetContentContainer();

        // Alpha Mode dropdown
        int alphaModeIdx = 0;
        if (Editor::MaterialRows::IsLightingModelShadowOnly(doc.lightingModel)) alphaModeIdx = 1;
        else if (doc.alphaMode == MaterialAlphaMode::Blend) alphaModeIdx = 1;
        else if (doc.alphaMode == MaterialAlphaMode::Mask) alphaModeIdx = 2;

        Dropdown* alphaDD = InspectorUI::AddDropdownRow(content, "Alpha Mode",
            {{"Opaque", "Opaque"}, {"Blend", "Blend"}, {"Mask", "Mask"}}, alphaModeIdx);

        alphaDD->SetOnValueChanged([root, mat, openPicker, undo, inlineEmbed, openAsset](const std::string& value) {
            MaterialDocument d = mat->GetDocument();
            d.alphaMode = Editor::MaterialRows::IsLightingModelShadowOnly(d.lightingModel)
                ? MaterialAlphaMode::Blend
                : MaterialAlphaModeFromString(value);
            if (undo)
            {
                Editor::UndoRedoService::InteractiveEdit edit =
                    undo->BeginInteractiveEdit("Material alphaMode",
                                               MakeMaterialSnapshotTarget(mat, "alphaMode"));
                if (edit)
                {
                    SaveAndInvalidate(mat, d);
                    edit.Commit();
                    RequestRebuild(root, mat, openPicker, {}, undo, inlineEmbed, openAsset);
                    return;
                }
            }
            SaveReloadRebuild(root, mat, d, openPicker, {}, undo, inlineEmbed, openAsset);
        });

        // Double Sided toggle
        AddMaterialToggleRow(content, "Double Sided", doc.doubleSided, root, mat, "doubleSided",
                             openPicker, undo, inlineEmbed, openAsset);

        // Texture filter dropdown — controls how bound textures are sampled.
        int filterIdx = 0;
        if (doc.textureFilter == MaterialTextureFilter::Bilinear) filterIdx = 1;
        else if (doc.textureFilter == MaterialTextureFilter::Point) filterIdx = 2;

        Dropdown* filterDD = InspectorUI::AddDropdownRow(content, "Texture Filter",
            {{"Trilinear", "Trilinear"}, {"Bilinear", "Bilinear"}, {"Point", "Point"}}, filterIdx);

        filterDD->SetOnValueChanged([root, mat, openPicker, undo, inlineEmbed, openAsset](const std::string& value) {
            MaterialDocument d = mat->GetDocument();
            d.textureFilter = MaterialTextureFilterFromString(value);
            if (undo)
            {
                Editor::UndoRedoService::InteractiveEdit edit =
                    undo->BeginInteractiveEdit("Material textureFilter",
                                               MakeMaterialSnapshotTarget(mat, "textureFilter"));
                if (edit)
                {
                    SaveAndInvalidate(mat, d);
                    edit.Commit();
                    RequestRebuild(root, mat, openPicker, {}, undo, inlineEmbed, openAsset);
                    PushRuntimeMaterialTextureBindings(mat);
                    return;
                }
            }
            SaveReloadRebuild(root, mat, d, openPicker, {}, undo, inlineEmbed, openAsset);
            PushRuntimeMaterialTextureBindings(mat);
        });

        root->AddChild(std::move(section));
    }

    // Declared properties: the surface's `// @property` table, from the same
    // cache the composer and material registration read. A declared surface
    // renders its rows from the table; a surface still read by lane name keeps
    // the built-in row model until it declares its properties.
    std::shared_ptr<const Rendering::ShaderPropertyTable> declared;
    bool surfaceProjectOwned = false;
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        declared = rs->Materials().ResolveDeclaredProperties(doc, mat->GetPath());
        surfaceProjectOwned = rs->Materials().IsSurfaceProjectOwned(doc, mat->GetPath());
    }
    const bool declaredSurface = declared && declared->HasSurfaceDeclarations && !declared->Rejected();

    // ========================================================================
    // [v] Properties section
    // ========================================================================
    {
        std::unique_ptr<Foldout> section = Editor::MakeMaterialSection("Properties", true);
        UIElement* content = section->GetContentContainer();

        if (declared && declared->Rejected())
        {
            // Diagnostics keep their absolute file (the panel parses and opens
            // it); the block DISPLAYS the project-relative spelling so the
            // author reads the path they authored, not a machine-local
            // staging or mirror tree.
            const Editor::EditorGlobalPaths globalPaths = Editor::GetEditorGlobalPaths();
            const ShaderErrorPathRoots displayRoots{
                EngineCore::GetInstance().GetAssetManager().GetAssetRoot(),
                {globalPaths.installAssetsRoot, globalPaths.userAssetsRoot}};
            std::ostringstream oss;
            oss << "Property declarations failed; the last good shader is still running:\n";
            for (const auto& e : declared->Errors)
                oss << " - " << ShaderErrorDisplayPath(e.File, displayRoots) << ":" << e.Line
                    << ": error: " << e.Message << "\n";
            Editor::AddMaterialLine(content, oss.str());
        }

        if (declaredSurface || (surfaceProjectOwned && declared && declared->Rejected()))
        {
            // A project surface renders only its declared rows. When the table is
            // rejected the error block above is the content — the hand-table rows
            // would edit keys the (still running) last-good shader may not read.
            if (declaredSurface)
                AddDeclaredPropertyRows(content, *declared, doc, root, mat, openPicker, undo, inlineEmbed,
                                        openAsset);
            root->AddChild(std::move(section));
        }
        else
        {
        const std::unordered_set<std::string> shaderGraphAliasedKeys =
            CollectShaderGraphAliasedPropertyKeys(doc);

        for (const auto& key : Editor::MaterialRows::BuiltInPropertyOrder())
        {
            if (shaderGraphAliasedKeys.count(key) != 0)
                continue;

            if (!Editor::MaterialRows::IsPropertyVisible(key, doc))
                continue;

            // A Mask material that never authored `alphaCutoff` still alpha-tests
            // against the registration default, so the row has to show that
            // effective value rather than vanish — the silently-applied threshold
            // is exactly what an author cannot otherwise see. The seed is
            // display-only: the edit handlers re-read the document from the asset,
            // so nothing is written until the author moves the slider.
            MaterialValue defaultedVal{};
            const MaterialValue* valPtr = nullptr;
            if (auto it = doc.properties.find(key); it != doc.properties.end())
                valPtr = &it->second;
            else if (key == "alphaCutoff")
            {
                defaultedVal = kDefaultAlphaCutoff;
                valPtr = &defaultedVal;
            }
            if (!valPtr)
                continue;

            const MaterialValue& val = *valPtr;

            if (Editor::MaterialRows::IsColorProperty(key) && std::holds_alternative<std::vector<float>>(val))
            {
                // emissive is a plain colour tint (its [0,1] chromaticity) like the other colour
                // rows; brightness is the separate "Emission Luminance (nits)" row, so the colour
                // picker never touches the nits value.
                const auto& arr = std::get<std::vector<float>>(val);
                AddColorSwatchRow(content, Editor::MaterialRows::PropertyLabel(key), arr, root, mat, key, openPicker, inlineEmbed, openAsset);
            }
            else if (key == "enableClearCoat" && std::holds_alternative<bool>(val))
            {
                // Toggle: writes the bool + rebuilds, which re-derives the ClearCoat
                // keyword in ApplyDocumentToMaterial (compiles the lobe in/out).
                AddMaterialToggleRow(content, "Clear Coat", std::get<bool>(val), root, mat,
                                     "enableClearCoat", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "enableSheen" && std::holds_alternative<bool>(val))
            {
                AddMaterialToggleRow(content, "Sheen", std::get<bool>(val), root, mat,
                                     "enableSheen", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "enableFuzz" && std::holds_alternative<bool>(val))
            {
                // Toggle: writes the bool + rebuilds, which re-derives the Fuzz keyword in
                // ApplyDocumentToMaterial (compiles the over-coat fuzz lobe in/out).
                AddMaterialToggleRow(content, "Fuzz", std::get<bool>(val), root, mat,
                                     "enableFuzz", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "enableAnisotropy" && std::holds_alternative<bool>(val))
            {
                AddMaterialToggleRow(content, "Anisotropy", std::get<bool>(val), root, mat,
                                     "enableAnisotropy", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "anisotropy")
            {
                // Signed [-1,1] slider — NOT the 0..1 IsSlider01Property path (which would clamp the negative half).
                float cur = 0.0f;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                AddMaterialSliderRow(content, "Anisotropy", cur, -1.0f, 1.0f, root, mat, key, openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "anisotropyRotation")
            {
                // Stored and consumed (uParams11.y) in radians, so the slider authors radians directly
                // — the inspector has no degrees-mapped angle controls to mirror, and the slider writes
                // the document value verbatim. Range 0..2*pi covers a full in-plane revolution.
                float cur = 0.0f;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                constexpr float kTwoPi = 6.28318530718f;
                AddMaterialSliderRow(content, "Anisotropy Rotation", cur, 0.0f, kTwoPi, root, mat, key, openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "enableSubsurface" && std::holds_alternative<bool>(val))
            {
                AddMaterialToggleRow(content, "Subsurface", std::get<bool>(val), root, mat,
                                     "enableSubsurface", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "enableTransmission" && std::holds_alternative<bool>(val))
            {
                AddMaterialToggleRow(content, "Transmission", std::get<bool>(val), root, mat,
                                     "enableTransmission", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "enableTransmissionThick" && std::holds_alternative<bool>(val))
            {
                AddMaterialToggleRow(content, "Thick (crystal ball)", std::get<bool>(val), root, mat,
                                     "enableTransmissionThick", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "refractionDistance")
            {
                // World-unit thick-lens path length — a 0..5 m slider, not the [0,1] IsSlider01 path.
                float cur = 0.0f;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                AddMaterialSliderRow(content, "Refraction Distance", cur, 0.0f, 5.0f, root, mat, key, openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "attenuationDistance")
            {
                // Beer–Lambert reference distance in world units — the depth at which transmitted
                // light reaches attenuationColor. A 0..5 m slider (0 = absorption off), not [0,1].
                float cur = 0.0f;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                AddMaterialSliderRow(content, "Attenuation Distance", cur, 0.0f, 5.0f, root, mat, key, openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "enableIridescence" && std::holds_alternative<bool>(val))
            {
                AddMaterialToggleRow(content, "Iridescence", std::get<bool>(val), root, mat,
                                     "enableIridescence", openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "thinFilmThickness")
            {
                // Film thickness in nanometres — a 0..2000 nm slider (the visible interference
                // band), not the [0,1] IsSlider01 path. thinFilmIor falls through the generic
                // float row below; thinFilmWeight uses the 0..1 IsSlider01 path.
                float cur = 0.0f;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                AddMaterialSliderRow(content, "Film Thickness (nm)", cur, 0.0f, 2000.0f, root, mat, key, openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "clearCoatIor")
            {
                // Coat refractive index — a 1.0..2.5 dielectric slider (the supported range; it
                // drives the coat Fresnel and the coat-darkening internal reflectance), bounding
                // the field instead of the unbounded generic float row.
                float cur = 1.5f;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                AddMaterialSliderRow(content, "Clear Coat IOR", cur, 1.0f, 2.5f, root, mat, key, openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "emissiveExposureWeight")
            {
                float cur = kDefaultEmissiveExposureWeight;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                AddMaterialSliderRow(content, Editor::MaterialRows::PropertyLabel(key), cur, 0.0f, 1.0f, root, mat, key,
                                     openPicker, undo, inlineEmbed, openAsset,
                                     Editor::MaterialRows::kEmissiveExposureWeightTooltip);
            }
            else if (Editor::MaterialRows::IsSlider01Property(key))
            {
                float cur = 0.0f;
                if (!TryGetScalarFromMaterialValue(val, cur))
                    continue;
                AddMaterialSliderRow(content, Editor::MaterialRows::PropertyLabel(key), cur, 0.0f, 1.0f, root, mat, key, openPicker, undo, inlineEmbed, openAsset);
            }
            else if (key == "emissionLuminance" && std::holds_alternative<float>(val))
            {
                // Emission in physical luminance (nits); 203 (reference white) == scene-linear 1.0.
                float cur = std::get<float>(val);
                InspectorDrag::AddFloatRowWithDrag(content, "Emission Luminance (nits)", cur,
                    [mat, key](float v) { UpdateRuntimeProperty(mat, key, MaterialValue{v}); },
                    [root, mat, key, openPicker, inlineEmbed, openAsset](float v) {
                        MaterialDocument d = mat->GetDocument();
                        d.properties[key] = v;
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    },
                    std::numeric_limits<float>::quiet_NaN(),
                    "Brightness of the emission in nits; it multiplies the emissive color and map. 0 emits "
                    "nothing; 203 is reference white.");
            }
            else if (std::holds_alternative<float>(val))
            {
                float cur = std::get<float>(val);
                InspectorDrag::AddFloatRowWithDrag(content, Editor::MaterialRows::PropertyLabel(key), cur,
                    [mat, key](float v) { UpdateRuntimeProperty(mat, key, MaterialValue{v}); },
                    [root, mat, key, openPicker, inlineEmbed, openAsset](float v) {
                        MaterialDocument d = mat->GetDocument();
                        d.properties[key] = v;
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    });
            }
        }

        if (!doc.shaderGraphPublicProperties.empty())
        {
            Editor::AddMaterialLine(content, "Shader Graph Properties");

            for (const MaterialShaderGraphPublicProperty& graphProp : doc.shaderGraphPublicProperties)
            {
                const std::string label =
                    ShaderGraph::PrettyGraphPropertyLabel(graphProp.GraphName);
                if ((graphProp.Type == "vec3" || graphProp.Type == "vec4") &&
                    graphProp.MaterialPropertyKeys.size() >= 3)
                {
                    AddShaderGraphAliasedColorRow(content, label, graphProp.MaterialPropertyKeys, root,
                                                  mat, openPicker, inlineEmbed, openAsset);
                }
                else if (graphProp.Type == "float" && graphProp.MaterialPropertyKeys.size() == 1)
                {
                    const std::string& propKey = graphProp.MaterialPropertyKeys.front();
                    auto it = doc.properties.find(propKey);
                    if (it == doc.properties.end())
                        continue;
                    if (!std::holds_alternative<float>(it->second))
                        continue;
                    const float cur = std::get<float>(it->second);
                    InspectorDrag::AddFloatRowWithDrag(content, label, cur,
                        [mat, propKey](float v) { UpdateRuntimeProperty(mat, propKey, MaterialValue{v}); },
                        [root, mat, propKey, openPicker, inlineEmbed, openAsset](float v) {
                            MaterialDocument d = mat->GetDocument();
                            d.properties[propKey] = v;
                            SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                        });
                }
            }
        }

        // Custom properties (not in the built-in set) exist for the engine and
        // package surfaces the hand-typed lane table still serves (triplanar*,
        // gpuFog*, wind*). A project surface exposes parameters only by
        // declaring them, so its unmapped keys get the undeclared notice below
        // instead of rows that would write to no lane.
        if (surfaceProjectOwned)
        {
            std::vector<std::string> unmappedKeys;
            for (const auto& kv : doc.properties)
            {
                if (Editor::MaterialRows::BuiltInPropertyNames().count(kv.first))
                    continue;
                if (shaderGraphAliasedKeys.count(kv.first) != 0)
                    continue;
                unmappedKeys.push_back(kv.first);
            }
            Editor::AddUndeclaredKeysNotice(content, unmappedKeys, {}, nullptr);
        }
        bool hasCustom = false;
        for (const auto& kv : doc.properties)
        {
            if (surfaceProjectOwned)
                break;
            if (Editor::MaterialRows::BuiltInPropertyNames().count(kv.first))
                continue;
            if (shaderGraphAliasedKeys.count(kv.first) != 0)
                continue;

            if (!hasCustom)
            {
                Editor::AddMaterialLine(content, "Custom Properties");
                hasCustom = true;
            }

            const std::string& propKey = kv.first;
            const MaterialValue& val = kv.second;

            const std::string propLabel = Editor::MaterialRows::PropertyLabel(propKey);
            if (std::holds_alternative<float>(val))
            {
                float cur = std::get<float>(val);
                InspectorDrag::AddFloatRowWithDrag(content, propLabel, cur,
                    [mat, propKey](float v) { UpdateRuntimeProperty(mat, propKey, MaterialValue{v}); },
                    [root, mat, propKey, openPicker, inlineEmbed, openAsset](float v) {
                        MaterialDocument d = mat->GetDocument();
                        d.properties[propKey] = v;
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    });
            }
            else if (std::holds_alternative<int32_t>(val))
            {
                int cur = (int)std::get<int32_t>(val);
                InspectorDrag::AddIntRowWithDrag(content, propLabel, cur,
                    [mat, propKey](int v) {
                        UpdateRuntimeProperty(mat, propKey, MaterialValue{static_cast<int32_t>(v)});
                    },
                    [root, mat, propKey, openPicker, inlineEmbed, openAsset](int v) {
                        MaterialDocument d = mat->GetDocument();
                        d.properties[propKey] = (int32_t)v;
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    });
            }
            else if (std::holds_alternative<bool>(val))
            {
                bool cur = std::get<bool>(val);
                InspectorDrag::AddToggleRow(content, propLabel, cur,
                    [root, mat, propKey, openPicker, inlineEmbed, openAsset](bool v) {
                        MaterialDocument d = mat->GetDocument();
                        d.properties[propKey] = v;
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    });
            }
            else if (std::holds_alternative<std::vector<float>>(val))
            {
                const auto& arr = std::get<std::vector<float>>(val);
                if (Editor::MaterialRows::IsColorProperty(propKey) && (arr.size() == 3 || arr.size() == 4))
                {
                    AddColorSwatchRow(content, propLabel, arr, root, mat, propKey, openPicker, inlineEmbed, openAsset);
                }
                else if (arr.size() == 3)
                {
                    Rendering::Vector3 cur(arr[0], arr[1], arr[2]);
                    auto row = std::make_unique<UIElement>();
                    row->AddClass("inspector-row");
                    UIElement* rowRaw = row.get();
                    content->AddChild(std::move(row));

                    auto label = std::make_unique<Label>();
                    label->AddClass("inspector-label");
                    label->SetText(propLabel);
                    rowRaw->AddChild(std::move(label));

                    auto field = std::make_unique<Vector3Field>();
                    field->SetValue(cur);
                    field->SetOnValueChanged([root, mat, propKey, openPicker, inlineEmbed, openAsset](const Rendering::Vector3& v) {
                        MaterialDocument d = mat->GetDocument();
                        d.properties[propKey] = std::vector<float>{v.x, v.y, v.z};
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    });
                    auto fieldContainer = std::make_unique<UIElement>();
                    fieldContainer->AddClass("inspector-field");
                    fieldContainer->AddChild(std::move(field));
                    rowRaw->AddChild(std::move(fieldContainer));
                }
                else
                {
                    // Generic float array → text field
                    auto row = std::make_unique<UIElement>();
                    row->AddClass("inspector-row");
                    UIElement* rowRaw = row.get();
                    content->AddChild(std::move(row));

                    auto label = std::make_unique<Label>();
                    label->AddClass("inspector-label");
                    label->SetText(propLabel);
                    rowRaw->AddChild(std::move(label));

                    auto field = std::make_unique<TextField>();
                    field->SetValue(FormatFloatList(arr));
                    size_t expectedSize = arr.size();
                    field->SetOnValueChanged([root, mat, propKey, expectedSize, openPicker, inlineEmbed, openAsset](const std::string& text) {
                        std::vector<float> parsed;
                        if (!TryParseFloatList(text, parsed) || parsed.size() != expectedSize)
                            return;
                        MaterialDocument d = mat->GetDocument();
                        d.properties[propKey] = parsed;
                        SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
                    });
                    auto fieldContainer = std::make_unique<UIElement>();
                    fieldContainer->AddClass("inspector-field");
                    fieldContainer->AddChild(std::move(field));
                    rowRaw->AddChild(std::move(fieldContainer));
                }
            }
        }

        root->AddChild(std::move(section));
        }
    }

    // ========================================================================
    // [v] Textures section
    // ========================================================================
    {
        std::unique_ptr<Foldout> section = Editor::MakeMaterialSection("Textures", true);
        UIElement* content = section->GetContentContainer();

        // Hex tiling is an engine-only feature with no OpenPBR equivalent, so it has no round-trip
        // home in a .mtlx — hide it for imported (shader-locked) materials. The texture slots below
        // stay visible so the imported maps are inspectable. Project surfaces read
        // none of the hex lanes, so they get no hex rows (declared or not).
        if (!doc.shaderLocked && !declaredSurface && !surfaceProjectOwned)
        {
            bool hexTilingEnabled = false;
            if (auto it = doc.properties.find("hexTiling"); it != doc.properties.end())
            {
                if (const bool* b = std::get_if<bool>(&it->second))
                    hexTilingEnabled = *b;
                else if (const int32_t* i = std::get_if<int32_t>(&it->second))
                    hexTilingEnabled = (*i != 0);
                else if (const float* f = std::get_if<float>(&it->second))
                    hexTilingEnabled = (*f != 0.0f);
            }
            AddMaterialToggleRow(content, "Hex Tiling", hexTilingEnabled, root, mat, "hexTiling",
                                 openPicker, undo, inlineEmbed, openAsset);

            float hexBlend = 0.5f;
            if (auto it = doc.properties.find("hexBlend"); it != doc.properties.end())
                (void)TryGetScalarFromMaterialValue(it->second, hexBlend);
            AddMaterialSliderRow(content, "Hex Blend", std::clamp(hexBlend, 0.0f, 1.0f), 0.0f, 1.0f,
                                 root, mat, "hexBlend", openPicker, undo, inlineEmbed, openAsset);

            float hexRotation = 1.0f;
            if (auto it = doc.properties.find("hexRotation"); it != doc.properties.end())
                (void)TryGetScalarFromMaterialValue(it->second, hexRotation);
            AddMaterialSliderRow(content, "Hex Rotation", std::clamp(hexRotation, 0.0f, 1.0f), 0.0f, 1.0f,
                                 root, mat, "hexRotation", openPicker, undo, inlineEmbed, openAsset);
        }

        // Known texture slots in display order
        auto onTextureChanged = [root, mat, openPicker, pingAsset, undo, inlineEmbed, openAsset](const std::string& texName, const GUID& guid) {
            MaterialDocument d = mat->GetDocument();
            if (guid.IsNull())
                d.textures[texName] = "";
            else
                d.textures[texName] = guid.ToString();

            // The coat normal is keyword-gated: assigning a map must compile the CoatNormal
            // variant in (and clearing it back out), mirroring how the lobe toggles drive
            // their keywords. Flip enableCoatNormal so the rebuild re-derives the keyword.
            if (texName == "coatNormalMap")
                d.properties["enableCoatNormal"] = !guid.IsNull();
            if (texName == "emissiveMap" && !guid.IsNull())
                Editor::MaterialRows::TurnOnEmissionForAssignedMap(d);

            if (undo)
            {
                Editor::UndoRedoService::InteractiveEdit edit = undo->BeginInteractiveEdit(
                    std::string("Material texture (") + texName + ')',
                    MakeMaterialSnapshotTarget(mat, texName));
                if (edit)
                {
                    SaveAndInvalidate(mat, d);
                    edit.Commit();
                    RequestRebuild(root, mat, openPicker, pingAsset, undo, inlineEmbed, openAsset);
                }
                else
                {
                    SaveReloadRebuild(root, mat, d, openPicker, pingAsset, undo, inlineEmbed, openAsset);
                }
            }
            else
            {
                SaveReloadRebuild(root, mat, d, openPicker, pingAsset, undo, inlineEmbed, openAsset);
            }

            // A texture not on the GPU loads through the bind's upload job (TextureService); the slot
            // samples its awaited default until then, so the assign never waits for a cook here.
            PushRuntimeMaterialTextureBindings(mat);
        };

        // Helper: add a 2x2 numeric grid for a texture slot's tiling/offset
        // transform, laid out as
        //
        //          Tiling     Offset
        //     x   [tilingX]  [offsetX]
        //     y   [tilingY]  [offsetY]
        //
        // Interactions on each cell:
        //   - Type into the field: live runtime update each keystroke,
        //     commit + undo on Enter/blur.
        // Interactions on the column / row header labels:
        //   - Drag horizontally: scrubs the two values that fall under that
        //     header together (Tiling drags both tilingX+tilingY,
        //     Offset drags both offsetX+offsetY, x drags tilingX+offsetX,
        //     y drags tilingY+offsetY).
        //   - Double-click: resets those two values to their defaults
        //     (1.0 for tiling components, 0.0 for offset components).
        // A single interactive-edit spans an entire drag/typing gesture so
        // undo collapses the whole gesture into one step.
        auto addTilingOffsetRows = [mat, &doc, undo](UIElement* parent, const std::string& texName) {
            // Read current values (identity if absent).
            std::array<float, 8> st = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
            auto stIt = doc.textureTransforms.find(texName);
            if (stIt != doc.textureTransforms.end())
                st = stIt->second;

            using OptEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
            auto editPtr = std::make_shared<OptEdit>();

            // Apply a single component change. Reads the persistent doc, mutates
            // the indexed slot, and pushes a runtime update (no disk write).
            auto previewComponent = [mat, texName](int idx, float v) {
                MaterialDocument d = mat->GetDocument();
                auto& s = d.textureTransforms[texName];
                if (s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f && s[4] == 0.0f && s[5] == 0.0f && s[6] == 0.0f)
                    s = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
                s[idx] = v;
                UpdateRuntimeTextureTransform(mat, texName, s);
            };

            // Apply two component changes atomically. Required for header drags
            // because previewComponent reads the persistent doc each call —
            // calling it twice in a row would clobber the first write.
            auto previewComponentTwo = [mat, texName](int idxA, float vA, int idxB, float vB) {
                MaterialDocument d = mat->GetDocument();
                auto& s = d.textureTransforms[texName];
                if (s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f && s[4] == 0.0f && s[5] == 0.0f && s[6] == 0.0f)
                    s = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
                s[idxA] = vA;
                s[idxB] = vB;
                UpdateRuntimeTextureTransform(mat, texName, s);
            };

            auto commitComponent = [mat, texName, editPtr](int idx, float v) {
                MaterialDocument d = mat->GetDocument();
                auto& s = d.textureTransforms[texName];
                if (s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f && s[4] == 0.0f && s[5] == 0.0f && s[6] == 0.0f)
                    s = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
                s[idx] = v;
                const std::array<float, 8> newSt = s;

                SaveDocToDisk(mat, d);
                (void)mat->Reload();
                UpdateRuntimeTextureTransform(mat, texName, newSt);

                if (editPtr && editPtr->has_value() && editPtr->value())
                {
                    editPtr->value().Commit();
                    editPtr->reset();
                }
            };

            auto beginEditIfNeeded = [mat, editPtr, undo](const std::string& label) {
                if (!undo || !editPtr)
                    return;
                if (editPtr->has_value())
                    return;
                editPtr->emplace(undo->BeginInteractiveEdit(label, MakeMaterialSnapshotTarget(mat, label)));
            };

            // Build the 2x2 grid container.
            auto gridOwned = std::make_unique<UIElement>();
            gridOwned->AddClass("material-tiling-grid");
            UIElement* grid = gridOwned.get();
            parent->AddChild(std::move(gridOwned));

            auto addGridRow = [grid](const char* extraClass) -> UIElement* {
                auto rowOwned = std::make_unique<UIElement>();
                rowOwned->AddClass("material-tiling-grid-row");
                if (extraClass)
                    rowOwned->AddClass(extraClass);
                UIElement* raw = rowOwned.get();
                grid->AddChild(std::move(rowOwned));
                return raw;
            };

            auto addHeaderLabel = [](UIElement* row, const char* text, const char* cls) -> Label* {
                auto lblOwned = std::make_unique<Label>();
                Label* lbl = lblOwned.get();
                lblOwned->SetText(text);
                lblOwned->AddClass("inspector-label");
                lblOwned->AddClass(cls);
                row->AddChild(std::move(lblOwned));
                return lbl;
            };

            auto addCellField = [&](UIElement* row, int idx, float val, float defaultValue,
                                     const std::string& editLabel) -> FloatField* {
                auto fOwned = std::make_unique<FloatField>();
                FloatField* fRaw = fOwned.get();
                fOwned->SetValue(val);
                fOwned->AddClass("material-tiling-cell");
                fOwned->SetOnValueChanging([previewComponent, beginEditIfNeeded, editLabel, idx](float v) {
                    beginEditIfNeeded(editLabel);
                    previewComponent(idx, v);
                });
                fOwned->SetOnValueChanged([commitComponent, beginEditIfNeeded, editLabel, idx](float v) {
                    beginEditIfNeeded(editLabel);
                    commitComponent(idx, v);
                });
                row->AddChild(std::move(fOwned));
                (void)defaultValue;
                return fRaw;
            };

            // Helper for the empty cell that aligns the offset header with its
            // column's row labels in the value rows. material-tiling-offset-col
            // gives it the 12px column gap.
            auto addCornerCell = [](UIElement* row, bool offsetColumn) {
                auto owned = std::make_unique<UIElement>();
                owned->AddClass("material-tiling-corner");
                if (offsetColumn)
                    owned->AddClass("material-tiling-offset-col");
                row->AddChild(std::move(owned));
            };

            // Header row: [corner] [Tiling] [corner] [Offset]
            UIElement* headerRow = addGridRow("material-tiling-header-row");
            addCornerCell(headerRow, false);
            Label* tilingHeader = addHeaderLabel(headerRow, "Tiling", "material-tiling-col-header");
            addCornerCell(headerRow, true);
            Label* offsetHeader = addHeaderLabel(headerRow, "Offset", "material-tiling-col-header");

            // X row: [x] [tilingX] [x] [offsetX]
            UIElement* xRow = addGridRow(nullptr);
            Label* xTilingLabel = addHeaderLabel(xRow, "x", "material-tiling-row-header");
            FloatField* tilingXField = addCellField(xRow, 0, st[0], 1.0f, "Material Tiling X");
            Label* xOffsetLabel = addHeaderLabel(xRow, "x", "material-tiling-row-header");
            xOffsetLabel->AddClass("material-tiling-offset-col");
            FloatField* offsetXField = addCellField(xRow, 2, st[2], 0.0f, "Material Offset X");

            // Y row: [y] [tilingY] [y] [offsetY]
            UIElement* yRow = addGridRow(nullptr);
            Label* yTilingLabel = addHeaderLabel(yRow, "y", "material-tiling-row-header");
            FloatField* tilingYField = addCellField(yRow, 5, st[5], 1.0f, "Material Tiling Y");
            Label* yOffsetLabel = addHeaderLabel(yRow, "y", "material-tiling-row-header");
            yOffsetLabel->AddClass("material-tiling-offset-col");
            FloatField* offsetYField = addCellField(yRow, 6, st[6], 0.0f, "Material Offset Y");

            // Drag-and-double-click on a header label scrubs/resets two cells
            // together. snapToWhole rounds to integers unless Alt is held, used
            // for the Tiling column header (whole-number tilings are typical).
            auto setupHeaderDrag = [previewComponentTwo, commitComponent, beginEditIfNeeded](
                Label* label, FloatField* fA, FloatField* fB,
                int idxA, int idxB, float defaultA, float defaultB,
                std::string editLabel, bool snapToWhole)
            {
                if (!label || !fA || !fB)
                    return;
                label->Overrides().Set(Style::Cursor, CursorStyle::ColResize);

                auto startA = std::make_shared<float>(0.0f);
                auto startB = std::make_shared<float>(0.0f);
                auto didMove = std::make_shared<bool>(false);
                auto lastClick = std::make_shared<std::chrono::steady_clock::time_point>();

                label->RegisterEventHandler(kEventMouseDown,
                    [label, fA, fB, startA, startB, didMove, lastClick,
                     defaultA, defaultB, idxA, idxB,
                     beginEditIfNeeded, commitComponent, editLabel](UIEvent& e) {
                    if (e.Button != 0)
                        return;
                    auto& state = InspectorDrag::GetDragState();
                    if (state.draggingLabel != nullptr)
                        return;

                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastClick);
                    if (lastClick->time_since_epoch().count() != 0 && elapsed < GameEngine::Platform::GetDoubleClickInterval()) {
                        beginEditIfNeeded(editLabel);
                        fA->SetValue(defaultA);
                        fB->SetValue(defaultB);
                        commitComponent(idxA, defaultA);
                        commitComponent(idxB, defaultB);
                        *lastClick = {};
                        e.Stop();
                        return;
                    }
                    *lastClick = now;

                    *didMove = false;
                    state.draggingLabel = label;
                    state.dragStartX = e.X;
                    *startA = fA->GetValue();
                    *startB = fB->GetValue();
                    InspectorDrag::SetLabelDragHighlight(label, true);
                    e.Capture(label);
                    e.Stop();
                });

                label->RegisterEventHandler(kEventMouseMove,
                    [label, fA, fB, startA, startB, didMove, snapToWhole, idxA, idxB,
                     beginEditIfNeeded, previewComponentTwo, editLabel](UIEvent& e) {
                    auto& state = InspectorDrag::GetDragState();
                    if (state.draggingLabel != label)
                        return;
                    float deltaX = e.X - state.dragStartX;
                    float newA = *startA + deltaX * InspectorDrag::kInspectorDragFloatSensitivity;
                    float newB = *startB + deltaX * InspectorDrag::kInspectorDragFloatSensitivity;
                    if (snapToWhole && !(e.Mods & Input::kModAlt)) {
                        newA = std::round(newA);
                        newB = std::round(newB);
                    }
                    fA->SetValue(newA);
                    fB->SetValue(newB);
                    *didMove = true;
                    beginEditIfNeeded(editLabel);
                    previewComponentTwo(idxA, newA, idxB, newB);
                    e.Stop();
                });

                label->RegisterEventHandler(kEventMouseUp,
                    [label, fA, fB, didMove, idxA, idxB, commitComponent](UIEvent& e) {
                    auto& state = InspectorDrag::GetDragState();
                    if (state.draggingLabel != label || e.Button != 0)
                        return;
                    InspectorDrag::SetLabelDragHighlight(label, false);
                    state.draggingLabel = nullptr;
                    if (*didMove) {
                        commitComponent(idxA, fA->GetValue());
                        commitComponent(idxB, fB->GetValue());
                    }
                    e.Stop();
                });
            };

            // Column headers: drag both axes of a single transform together.
            setupHeaderDrag(tilingHeader, tilingXField, tilingYField,
                            0, 5, 1.0f, 1.0f, "Material Tiling", true);
            setupHeaderDrag(offsetHeader, offsetXField, offsetYField,
                            2, 6, 0.0f, 0.0f, "Material Offset", false);

            // Per-cell drags: each x/y row label scrubs only its own column's
            // value (the previous row-spanning behavior was confusing — the
            // user expected x in the Tiling column to control just tilingX).
            // SetupLabelDragFloat also wires double-click-to-reset to the
            // supplied default value.
            auto setupCellDrag = [previewComponent, commitComponent, beginEditIfNeeded](
                Label* label, FloatField* field, int idx, float defaultValue,
                std::string editLabel)
            {
                InspectorDrag::SetupLabelDragFloat(label, field,
                    [field, previewComponent, beginEditIfNeeded, editLabel, idx]() {
                        beginEditIfNeeded(editLabel);
                        previewComponent(idx, field->GetValue());
                    },
                    [field, commitComponent, beginEditIfNeeded, editLabel, idx]() {
                        beginEditIfNeeded(editLabel);
                        commitComponent(idx, field->GetValue());
                    },
                    defaultValue);
            };
            setupCellDrag(xTilingLabel, tilingXField, 0, 1.0f, "Material Tiling X");
            setupCellDrag(yTilingLabel, tilingYField, 5, 1.0f, "Material Tiling Y");
            setupCellDrag(xOffsetLabel, offsetXField, 2, 0.0f, "Material Offset X");
            setupCellDrag(yOffsetLabel, offsetYField, 6, 0.0f, "Material Offset Y");
        };

        std::unordered_set<std::string> shown;

        // Inline material inspectors should ping assets without changing the
        // currently inspected material/mesh selection.
        auto textureSlotPingAsset = (inlineEmbed && pingAssetPreserveInspector)
                                        ? pingAssetPreserveInspector
                                        : pingAsset;

        // Build one texture slot block: optional divider, asset-field row, preview, tiling/offset.
        bool firstTextureBlock = true;
        auto addTextureBlock = [&](const std::string& texName, const GUID& currentGuid) {
            if (!firstTextureBlock)
            {
                auto divider = std::make_unique<UIElement>();
                divider->AddClass("material-texture-divider");
                content->AddChild(std::move(divider));
            }
            firstTextureBlock = false;

            UIElement* row = InspectorUI::AddRow(content);
            row->AddClass("material-texture-row");
            if (Label* lbl = InspectorUI::AddLabel(row, Editor::MaterialRows::TextureSlotLabel(texName)))
                lbl->AddClass("material-texture-name");

            // Ping button: navigates the Assets panel to the referenced texture.
            // Placed between the label and the field so it visually appears to
            // the left of the texture preview, matching the Model / Material
            // ping pattern in MeshRendererInspector.
            if (!currentGuid.IsNull() && textureSlotPingAsset)
            {
                AssetMetadata texMd{};
                if (registry.TryGetAssetMetadata(currentGuid, texMd) && !texMd.Path.empty())
                {
                    auto pingBtn = std::make_unique<Button>();
                    pingBtn->AddClass("icon-button");
                    pingBtn->AddClass("pin-icon");
                    pingBtn->SetTooltip("Reveal Texture in Assets");
                    auto pingFn = textureSlotPingAsset;
                    auto texPath = texMd.Path;
                    pingBtn->RegisterEventHandler(kEventButtonClick, [pingFn, texPath](UIEvent&) { pingFn(texPath); });
                    row->AddChild(std::move(pingBtn));
                }
            }

            UIElement* fc = InspectorUI::AddFieldContainer(row);

            auto field = std::make_unique<AssetField>();
            field->AddClass("dropdown-asset-field");
            field->SetAcceptedTypes({AssetType::Texture});
            field->SetAssetRegistry(&registry);
            field->SetValue(currentGuid);
            field->SetOnValueChanged([onTextureChanged, texName](const GUID& guid) {
                onTextureChanged(texName, guid);
            });
            fc->AddChild(std::move(field));

            // Place the tiling/offset 2x2 grid next to the texture preview on
            // the right. The CSS (.material-tiling-preview-row) uses
            // align-self:stretch — Foldout.css warns % widths are unreliable
            // in our CSS parser, so we rely on flex stretching instead.
            auto wrapOwned = std::make_unique<UIElement>();
            wrapOwned->AddClass("material-tiling-preview-row");
            UIElement* wrap = wrapOwned.get();
            content->AddChild(std::move(wrapOwned));

            addTilingOffsetRows(wrap, texName);

            auto previewHost = std::make_unique<MaterialTexturePreviewHost>();
            previewHost->AddClass("texture-preview");
            previewHost->AddClass("texture-preview-side");
            if (doc.textureFilter == MaterialTextureFilter::Point)
                previewHost->AddClass("ui-bg-point-filter");
            previewHost->SetAssetRegistry(&registry);
            previewHost->SetOnTextureDropped([onTextureChanged, texName](const GUID& guid) {
                onTextureChanged(texName, guid);
            });
            if (textureSlotPingAsset)
            {
                AssetRegistry* registryPtr = &registry;
                auto pingFn = textureSlotPingAsset;
                previewHost->SetOnTextureClicked([registryPtr, pingFn](const GUID& guid)
                {
                    if (!registryPtr || guid.IsNull())
                        return;
                    AssetMetadata md{};
                    if (!registryPtr->TryGetAssetMetadata(guid, md) || md.Path.empty())
                        return;
                    pingFn(md.Path);
                });
            }
            previewHost->SetTexturePreviewFromGuid(registry, currentGuid);
            wrap->AddChild(std::move(previewHost));
        };

        const Engine::Renderer::MaterialSystem::SurfaceTextureUse surfaceTextures = ResolveSurfaceTextureUse(mat, doc);
        for (const auto& texName : Editor::MaterialRows::TextureSlotOrder())
        {
            if (!Editor::MaterialRows::IsTextureVisible(texName, doc, surfaceTextures.DeclaredNames))
                continue;

            auto it = doc.textures.find(texName);

            // Resolve current GUID
            GUID currentGuid = GUID::Null();
            if (it != doc.textures.end() && !it->second.empty())
            {
                currentGuid = GUID(it->second);
            }

            addTextureBlock(texName, currentGuid);
            if (texName == Rendering::kParallaxHeightMapSlot)
                AddHeightMapRows(content, doc, surfaceTextures, root, mat, openPicker, undo, inlineEmbed,
                                 openAsset);
            shown.insert(texName);
        }

        // Any additional textures not in the known list. A height map bound on a surface without a
        // height slot lands here, with the refusal that says where it belongs.
        for (const auto& kv : doc.textures)
        {
            if (shown.count(kv.first))
                continue;

            GUID currentGuid = GUID::Null();
            if (!kv.second.empty())
                currentGuid = GUID(kv.second);

            addTextureBlock(kv.first, currentGuid);
            if (kv.first == Rendering::kParallaxHeightMapSlot)
                AddHeightMapRows(content, doc, surfaceTextures, root, mat, openPicker, undo, inlineEmbed,
                                 openAsset);
        }

        root->AddChild(std::move(section));
    }

    // ========================================================================
    // [>] Advanced Shader section (collapsed by default). Hidden for imported (MaterialX)
    // materials, whose shader is pinned to StandardPBR (no surface-shader/graph swapping).
    // ========================================================================
    if (!doc.shaderLocked)
    {
        std::unique_ptr<Foldout> section = Editor::MakeMaterialSection("Advanced Shader", false);
        UIElement* content = section->GetContentContainer();

        // Surface Graph (material node graph -> auto-generated surface shader)
        GUID surfaceGraphGuid = GUID::Null();
        if (!doc.surfaceGraphGuid.empty())
            surfaceGraphGuid = GUID(doc.surfaceGraphGuid);

        InspectorUI::AddAssetFieldRow(content, "Surface Graph", surfaceGraphGuid,
            {AssetType::Graph}, &registry,
            [root, mat, openPicker, inlineEmbed, openAsset](const GUID& guid) {
                MaterialDocument d = mat->GetDocument();
                d.surfaceGraphGuid = guid.IsNull() ? std::string() : guid.ToString();
                d.surfaceGraph.clear();
                if (!guid.IsNull())
                {
                    d.surfaceShaderGuid.clear();
                    d.surfaceShader.clear();
                }
                if (auto* rs = EngineCore::GetInstance().GetRenderServices())
                    rs->Materials().Compiler().ReconcileShaderReferences(d);

                SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
            });

        if (!doc.surfaceGraphGuid.empty() || !doc.surfaceGraph.empty())
        {
            UIElement* buttonRow = InspectorUI::AddRow(content);
            auto openGraphBtn = std::make_unique<Button>();
            openGraphBtn->AddClass("rp-single-line-button");
            openGraphBtn->SetText("Select Graph in Browser");
            openGraphBtn->RegisterEventHandler(kEventButtonClick, [mat, pingAsset](UIEvent&) {
                const std::filesystem::path graphPath =
                    ResolveMaterialSurfaceGraphPath(mat, mat->GetDocument());
                if (graphPath.empty())
                    return;
                if (pingAsset)
                    pingAsset(graphPath);
                else
                    Platform::OpenPath(graphPath);
            });
            buttonRow->AddChild(std::move(openGraphBtn));

            if (openAsset)
            {
                const std::filesystem::path graphPath = ResolveMaterialSurfaceGraphPath(mat, doc);
                if (!graphPath.empty())
                {
                    UIElement* graphButtonRow = InspectorUI::AddRow(content);
                    auto graphEditorBtn = std::make_unique<Button>();
                    graphEditorBtn->AddClass("rp-single-line-button");
                    graphEditorBtn->SetText("Open in Material Graph");
                    graphEditorBtn->RegisterEventHandler(kEventButtonClick, [openAsset, graphPath](UIEvent&) {
                        openAsset(graphPath);
                    });
                    graphButtonRow->AddChild(std::move(graphEditorBtn));
                }
            }
        }

        // Surface Shader (GUID-based AssetField). A document that names its shader
        // by path only — a pair rename drops the GUID — shows the asset at that path.
        const std::filesystem::path surfaceShaderPath = ResolveMaterialSurfaceShaderPath(mat, doc);
        GUID surfaceGuid = GUID::Null();
        if (!doc.surfaceShaderGuid.empty())
            surfaceGuid = GUID(doc.surfaceShaderGuid);
        else if (!surfaceShaderPath.empty())
            surfaceGuid = registry.GetAssetGUID(surfaceShaderPath);

        InspectorUI::AddAssetFieldRow(content, "Surface Shader", surfaceGuid,
            {AssetType::Shader}, &registry,
            [root, mat, openPicker, inlineEmbed, openAsset](const GUID& guid) {
                MaterialDocument d = mat->GetDocument();
                d.surfaceShaderGuid = guid.IsNull() ? std::string() : guid.ToString();
                d.surfaceShader.clear();
                if (!guid.IsNull())
                {
                    d.surfaceGraphGuid.clear();
                    d.surfaceGraph.clear();
                }
                if (auto* rs = EngineCore::GetInstance().GetRenderServices())
                    rs->Materials().Compiler().ReconcileShaderReferences(d);

                if (!guid.IsNull() && d.vertexModifierGuid.empty() && !d.surfaceShader.empty())
                {
                    using Rendering::ShaderCapabilityDetector;
                    using Rendering::ShaderCapability;
                    using Rendering::HasCapability;
                    auto caps = ShaderCapabilityDetector::DetectFromFile(d.surfaceShader);
                    if (HasCapability(caps, ShaderCapability::VertexModifier))
                    {
                        d.vertexModifierGuid = d.surfaceShaderGuid;
                        d.vertexModifier = d.surfaceShader;
                    }
                }

                SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
            });

        // Opens the surface the way a double-click in the Assets browser does: the
        // Script Editor for a hand-written surface, the Material Graph for a
        // graph-generated one. Without a host policy the OS default app is the fallback.
        if (!doc.surfaceShaderGuid.empty() || !doc.surfaceShader.empty())
        {
            UIElement* buttonRow = InspectorUI::AddRow(content);
            auto openBtn = std::make_unique<Button>();
            openBtn->AddClass("rp-single-line-button");
            openBtn->SetText("Open Surface Shader");
            openBtn->RegisterEventHandler(kEventButtonClick, [openAsset, surfaceShaderPath](UIEvent&) {
                if (surfaceShaderPath.empty())
                    return;
                if (openAsset)
                    openAsset(surfaceShaderPath);
                else
                    Platform::OpenPath(surfaceShaderPath);
            });
            buttonRow->AddChild(std::move(openBtn));
        }

        // Vertex Modifier (GUID-based AssetField)
        GUID vertModGuid = GUID::Null();
        if (!doc.vertexModifierGuid.empty())
            vertModGuid = GUID(doc.vertexModifierGuid);

        InspectorUI::AddAssetFieldRow(content, "Vertex Modifier", vertModGuid,
            {AssetType::Shader}, &registry,
            [root, mat, openPicker, inlineEmbed, openAsset](const GUID& guid) {
                MaterialDocument d = mat->GetDocument();
                d.vertexModifierGuid = guid.IsNull() ? std::string() : guid.ToString();
                d.vertexModifier.clear();
                if (auto* rs = EngineCore::GetInstance().GetRenderServices())
                    rs->Materials().Compiler().ReconcileShaderReferences(d);

                if (!guid.IsNull() && d.surfaceShaderGuid.empty() && !d.vertexModifier.empty())
                {
                    using Rendering::ShaderCapabilityDetector;
                    using Rendering::ShaderCapability;
                    using Rendering::HasCapability;
                    auto caps = ShaderCapabilityDetector::DetectFromFile(d.vertexModifier);
                    if (HasCapability(caps, ShaderCapability::Surface))
                    {
                        d.surfaceShaderGuid = d.vertexModifierGuid;
                        d.surfaceShader = d.vertexModifier;
                    }
                }

                SaveReloadRebuild(root, mat, d, openPicker, {}, nullptr, inlineEmbed, openAsset);
            });

        root->AddChild(std::move(section));
    }

    // ========================================================================
    // [>] Shader Diagnostics section (collapsed unless errors)
    // ========================================================================
    {
        // Snapshot the compile result. The shared_ptr owns the data so the
        // worker thread that produces the next result can swap the cache
        // entry without invalidating what we read here.
        std::shared_ptr<const Engine::Renderer::MaterialCompiler::Result> compileResult;
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            compileResult = rs->Materials().Compiler().Get(mat->GetGUID());
        }

        bool hasCompileErrors = compileResult && compileResult->hasResult && !compileResult->success;

        // Collapsed for the healthy case; a failed compile expands it so the
        // error list is on screen the moment the material is selected instead
        // of hiding behind a closed foldout.
        std::unique_ptr<Foldout> section = Editor::MakeMaterialSection("Shader Diagnostics", hasCompileErrors);
        UIElement* content = section->GetContentContainer();

        // Red error dot in the header when compile failed.
        if (hasCompileErrors)
        {
            // The foldout's first internal child is the header row (flex row).
            auto& internalChildren = section->UIElement::GetChildren();
            UIElement* header = internalChildren.empty() ? nullptr : internalChildren[0].get();
            if (header)
            {
                auto dot = std::make_unique<UIElement>();
                dot->AddClass("error-dot");
                header->AddChild(std::move(dot));
            }
        }

        // Compile status
        {
            const char* status = "Shader: FAILED";
            if (!compileResult || !compileResult->hasResult)
                status = "Shader: compiling…";
            else if (compileResult->success)
                status = compileResult->stale ? "Shader: OK (stale)" : "Shader: OK";
            Editor::AddMaterialLine(content, status);
        }

        // Compile errors
        if (compileResult && compileResult->hasResult && !compileResult->errors.empty())
        {
            std::ostringstream oss;
            for (const auto& e : compileResult->errors)
                oss << " - " << e << "\n";
            Editor::AddMaterialLine(content, oss.str());
        }

        // Shader meta validation (if available)
        if (compileResult && compileResult->shaderMeta)
        {
            const Rendering::ShaderMeta* shaderMeta = compileResult->shaderMeta.get();

            auto shaderReport = Rendering::ValidateShaderMeta(*shaderMeta, 128);
            if (!shaderReport.Issues.empty())
            {
                std::ostringstream oss;
                oss << "Shader validation:\n";
                for (const auto& i : shaderReport.Issues)
                {
                    const char* sev =
                        (i.Severity == Rendering::IssueSeverity::Error) ? "ERROR"
                        : (i.Severity == Rendering::IssueSeverity::Warning) ? "WARN" : "INFO";
                    oss << " [" << sev << "] " << i.Code << ": " << i.Message << "\n";
                }
                Editor::AddMaterialLine(content, oss.str());
            }

            // Material vs meta validation
            Rendering::MaterialValidationInput mvi{};
            for (const auto& kv : doc.properties)
                mvi.propertyNames.push_back(kv.first);
            for (const auto& kv : doc.textures)
                mvi.textureNames.push_back(kv.first);
            mvi.bindings = doc.bindings;

            auto matReport = Rendering::ValidateMaterialAgainstMeta(*shaderMeta, mvi);
            if (!matReport.Issues.empty())
            {
                std::ostringstream oss;
                oss << "Material validation:\n";
                for (const auto& i : matReport.Issues)
                {
                    const char* sev =
                        (i.Severity == Rendering::IssueSeverity::Error) ? "ERROR"
                        : (i.Severity == Rendering::IssueSeverity::Warning) ? "WARN" : "INFO";
                    oss << " [" << sev << "] " << i.Code << ": " << i.Message << "\n";
                }
                Editor::AddMaterialLine(content, oss.str());
            }
        }

        // Recompile button — a manual fallback. Saved .glsl edits propagate
        // automatically (MaterialSystem's shader-source edit lane); this exists
        // for cases the watcher can't see (network drives, tools that bypass
        // change notifications).
        {
            Editor::AddMaterialLine(content, "Shaders recompile automatically when the material or its\n"
                                             ".glsl sources change. Use Recompile if an external edit was missed.");

            auto recompileBtn = std::make_unique<Button>();
            recompileBtn->AddClass("inspector-text");
            recompileBtn->AddClass("material-line");
            recompileBtn->SetText("Recompile Shader");
            recompileBtn->RegisterEventHandler(kEventButtonClick, [root, mat, openPicker, pingAsset, inlineEmbed, openAsset](UIEvent&) {
                if (!mat)
                    return;
                if (auto* rs = EngineCore::GetInstance().GetRenderServices())
                {
                    rs->Materials().Compiler().Clear(mat->GetGUID());
                    rs->Materials().Compiler().RequestCompile(mat->GetGUID());
                    // Also push the edit through the runtime lane: recompile the
                    // registered pipelines of every material sharing this
                    // surface, so the viewport updates — not just the
                    // diagnostics. (Before this the button refreshed the
                    // inspector's compile result while draws kept the stale
                    // SPIR-V.)
                    const std::string& surface = mat->GetDocument().surfaceShader;
                    if (!surface.empty())
                        rs->Materials().NotifyShaderSourceEdited(surface);
                }
                RequestRebuild(root, mat, openPicker, pingAsset, nullptr, inlineEmbed, openAsset);
            });
            content->AddChild(std::move(recompileBtn));
        }

        root->AddChild(std::move(section));
    }

    // File path foldout at the very bottom of the inspector. Only shown when
    // embedded inline (e.g. inside a MeshRenderer foldout) so users can ping
    // the underlying asset; the standalone Material asset inspector already
    // lives at that file, so the path is redundant there.
    if (inlineEmbed)
    {
        std::unique_ptr<Foldout> foldout = Editor::MakeMaterialSection("File", false);

        UIElement* fileContent = foldout->GetContentContainer();

        auto pathRow = std::make_unique<UIElement>();
        pathRow->AddClass("inspector-row");

        auto pathLabel = std::make_unique<Label>();
        pathLabel->AddClass("inspector-text");
        pathLabel->SetText(mat->GetPath().string());

        if (pingAsset && !mat->GetPath().empty())
        {
            auto matPath = mat->GetPath();
            pathLabel->AddClass("clickable-text");
            pathLabel->RegisterEventHandler(kEventMouseDown, [pingAsset, matPath](UIEvent&) {
                pingAsset(matPath);
            });
        }
        pathRow->AddChild(std::move(pathLabel));

        fileContent->AddChild(std::move(pathRow));
        root->AddChild(std::move(foldout));
    }
}

void RegisterMaterialInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.Object)
            return;

        auto* base = static_cast<Asset*>(ctx.Object);
        auto* mat = dynamic_cast<MaterialAsset*>(base);
        if (!mat)
            return;

        auto rootOwned = std::make_unique<UIElement>();
        UIElement* root = rootOwned.get();
        ctx.Parent->AddChild(std::move(rootOwned));

        BuildMaterialInspectorUI(root, mat, ctx.OpenColorPickerWindow, ctx.PingAsset, ctx.Undo, false,
                                 ctx.OpenAsset);
        
        // Apply base color tint to the material inspector header icon
        const MaterialDocument& doc = mat->GetDocument();
        const uint32_t tint = BaseColorToIconTint(doc);
        if (tint != 0u && ctx.Parent)
        {
            // Find the inspector-section-icon element and apply the tint
            auto section = dynamic_cast<InspectorSection*>(ctx.Parent);
            if (section)
                section->SetHeaderIconTint(tint);
        }
    };

    InspectorRegistry::Get().RegisterAssetInspector(AssetType::Material, std::move(fn));
}

} // namespace GameEngine
