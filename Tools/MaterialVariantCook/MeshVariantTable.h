#pragma once

// The mesh-material variant table MaterialVariantCook cooks: every forward colour and depth
// variant a mesh material is asked for, across the four vertex buckets (static and skinned,
// each with and without tangents).

#include "VariantRequests.h"

namespace GameEngine::Tools::MaterialCook
{

using Rendering::MaterialKeyword;
using Rendering::VertexAttributeFlags;

// The variants a forward-rendered opaque material is asked for, mirroring the
// PipelineVariantCache requests the colour and depth passes make, across the
// four vertex buckets material prewarm covers. VariantsFor lists the
// vertex-colour twins of the rows that read the colour stream for the imported
// mesh materials (CookSource::ImportedMeshMaterial, ExpandVertexColorVariants).
//
// DECLARED, NOT DERIVED. The renderer chooses pass keywords per view, so this
// table is a claim about which views a cooked build supports — not a proof. A
// view that turns on a keyword outside it (GTAO, SSSR, ray-traced shadows,
// LOD crossfade) asks for a variant no cook produced, and the runtime says so
// by name rather than drawing wrong. Widen the table when the shipped pipeline
// grows a pass.
constexpr MaterialKeyword kForwardColor =
    MaterialKeyword::Instanced | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
constexpr MaterialKeyword kDepth = MaterialKeyword::Instanced;
// DepthDrawRecorder ORs this onto Mask depth/shadow/prepass so the fragment
// stage can discard. Opaque intern never sets the bit, so these rows are
// appended only for Mask documents — folding it into kDepth would miss-key
// every opaque depth intern.
constexpr MaterialKeyword kDepthMask = kDepth | MaterialKeyword::DepthOnlyFragment;
// The ForwardPlus graph runs IBLGen, so materials composed under it carry the
// IBL keyword; the non-IBL rows above stay for graphs without an environment.
constexpr MaterialKeyword kForwardColorIbl = kForwardColor | MaterialKeyword::IBL;
// The web player's Web.rendergraph world pass is kForwardColor (no IBL). A
// volume that activates SSSR re-keys every opaque material with the MRT
// keyword; without these rows the cooked-only runtime has nothing to draw.
constexpr MaterialKeyword kForwardColorSssr =
    kForwardColor | MaterialKeyword::SSSRNormalRoughness;
// SSSR runs on WebGPU where float32-filterable exists (20f0756e8), and the
// moment a volume activates it the colour pass re-keys every opaque material
// with the normal-roughness MRT keyword — a whole scene's geometry vanishes on
// a cooked-only runtime if these rows are missing.
constexpr MaterialKeyword kForwardColorIblSssr =
    kForwardColorIbl | MaterialKeyword::SSSRNormalRoughness;
// GTAO is the same shape: an AO volume sets the keyword per view and every
// opaque material re-keys to the variant that declares ge_gtao. Cooked with
// and without SSSR, because a scene may enable either or both.
constexpr MaterialKeyword kForwardColorIblGtao =
    kForwardColorIbl | MaterialKeyword::GTAO;
constexpr MaterialKeyword kForwardColorIblGtaoSssr =
    kForwardColorIblSssr | MaterialKeyword::GTAO;
constexpr MaterialKeyword kForwardColorIblDdgi = kForwardColorIbl | MaterialKeyword::DDGI;
constexpr MaterialKeyword kForwardColorIblDdgiSssr = kForwardColorIblSssr | MaterialKeyword::DDGI;
constexpr MaterialKeyword kForwardColorIblDdgiGtao = kForwardColorIblGtao | MaterialKeyword::DDGI;
constexpr MaterialKeyword kForwardColorIblDdgiGtaoSssr = kForwardColorIblGtaoSssr | MaterialKeyword::DDGI;

inline const VariantRequest kMeshVariants[] = {
    // The keyword-less base variant material registration prewarms before any
    // pass has asked for anything. Without it a material never gets its base
    // pipeline, whatever else is cooked.
    {"base", MaterialKeyword::None, VertexAttributeFlags::StandardMesh},

    {"forward-color", kForwardColor, VertexAttributeFlags::StandardMesh},
    {"forward-color+tangent", kForwardColor, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color+skin", kForwardColor, VertexAttributeFlags::SkinnedMesh},
    {"forward-color+skin+tangent", kForwardColor, VertexAttributeFlags::SkinnedMeshWithTangent},

    {"forward-color-sssr", kForwardColorSssr, VertexAttributeFlags::StandardMesh},
    {"forward-color-sssr+tangent", kForwardColorSssr, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-sssr+skin", kForwardColorSssr, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-sssr+skin+tangent", kForwardColorSssr, VertexAttributeFlags::SkinnedMeshWithTangent},

    {"forward-color-ibl", kForwardColorIbl, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl+tangent", kForwardColorIbl, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl+skin", kForwardColorIbl, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl+skin+tangent", kForwardColorIbl, VertexAttributeFlags::SkinnedMeshWithTangent},

    {"forward-color-ibl-sssr", kForwardColorIblSssr, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-sssr+tangent", kForwardColorIblSssr, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-sssr+skin", kForwardColorIblSssr, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-sssr+skin+tangent", kForwardColorIblSssr, VertexAttributeFlags::SkinnedMeshWithTangent},

    {"forward-color-ibl-gtao", kForwardColorIblGtao, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-gtao+tangent", kForwardColorIblGtao, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-gtao+skin", kForwardColorIblGtao, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-gtao+skin+tangent", kForwardColorIblGtao, VertexAttributeFlags::SkinnedMeshWithTangent},

    {"forward-color-ibl-ddgi", kForwardColorIblDdgi, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-ddgi+tangent", kForwardColorIblDdgi, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-ddgi+skin", kForwardColorIblDdgi, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-ddgi+skin+tangent", kForwardColorIblDdgi, VertexAttributeFlags::SkinnedMeshWithTangent},
    {"forward-color-ibl-ddgi-sssr", kForwardColorIblDdgiSssr, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-ddgi-sssr+tangent", kForwardColorIblDdgiSssr, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-ddgi-sssr+skin", kForwardColorIblDdgiSssr, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-ddgi-sssr+skin+tangent", kForwardColorIblDdgiSssr, VertexAttributeFlags::SkinnedMeshWithTangent},
    {"forward-color-ibl-ddgi-gtao", kForwardColorIblDdgiGtao, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-ddgi-gtao+tangent", kForwardColorIblDdgiGtao, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-ddgi-gtao+skin", kForwardColorIblDdgiGtao, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-ddgi-gtao+skin+tangent", kForwardColorIblDdgiGtao, VertexAttributeFlags::SkinnedMeshWithTangent},
    {"forward-color-ibl-ddgi-gtao-sssr", kForwardColorIblDdgiGtaoSssr, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-ddgi-gtao-sssr+tangent", kForwardColorIblDdgiGtaoSssr, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-ddgi-gtao-sssr+skin", kForwardColorIblDdgiGtaoSssr, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-ddgi-gtao-sssr+skin+tangent", kForwardColorIblDdgiGtaoSssr, VertexAttributeFlags::SkinnedMeshWithTangent},
    {"forward-color-ibl-gtao-sssr", kForwardColorIblGtaoSssr, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-gtao-sssr+tangent", kForwardColorIblGtaoSssr, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-gtao-sssr+skin", kForwardColorIblGtaoSssr, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-gtao-sssr+skin+tangent", kForwardColorIblGtaoSssr, VertexAttributeFlags::SkinnedMeshWithTangent},

    {"depth", kDepth, VertexAttributeFlags::StandardMesh},
    {"depth+tangent", kDepth, VertexAttributeFlags::StandardMeshWithTangent},
    {"depth+skin", kDepth, VertexAttributeFlags::SkinnedMesh},
    {"depth+skin+tangent", kDepth, VertexAttributeFlags::SkinnedMeshWithTangent},
};

// The variants an alpha-blended mesh material is asked for: the keyword-less base variant its
// registration prewarms (without it the material never gets a base pipeline, and so never draws),
// the world pass the web draws it in (IBL; IBL with DDGI; and IBL with DDGI and SSSR's
// normal-roughness output, which a view lit by a DDGI volume asks for) and depth, across the four
// vertex buckets material prewarm covers. Narrower than kMeshVariants: the blended stand-in
// cooks only what the web requests of it.
inline const VariantRequest kBlendMeshVariants[] = {
    {"base", MaterialKeyword::None, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl", kForwardColorIbl, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl+tangent", kForwardColorIbl, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl+skin", kForwardColorIbl, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl+skin+tangent", kForwardColorIbl, VertexAttributeFlags::SkinnedMeshWithTangent},
    {"forward-color-ibl-ddgi", kForwardColorIblDdgi, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-ddgi+tangent", kForwardColorIblDdgi, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-ddgi+skin", kForwardColorIblDdgi, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-ddgi+skin+tangent", kForwardColorIblDdgi, VertexAttributeFlags::SkinnedMeshWithTangent},
    {"forward-color-ibl-ddgi-sssr", kForwardColorIblDdgiSssr, VertexAttributeFlags::StandardMesh},
    {"forward-color-ibl-ddgi-sssr+tangent", kForwardColorIblDdgiSssr, VertexAttributeFlags::StandardMeshWithTangent},
    {"forward-color-ibl-ddgi-sssr+skin", kForwardColorIblDdgiSssr, VertexAttributeFlags::SkinnedMesh},
    {"forward-color-ibl-ddgi-sssr+skin+tangent", kForwardColorIblDdgiSssr, VertexAttributeFlags::SkinnedMeshWithTangent},
    {"depth", kDepth, VertexAttributeFlags::StandardMesh},
    {"depth+tangent", kDepth, VertexAttributeFlags::StandardMeshWithTangent},
    {"depth+skin", kDepth, VertexAttributeFlags::SkinnedMesh},
    {"depth+skin+tangent", kDepth, VertexAttributeFlags::SkinnedMeshWithTangent},
};

// The standard-PBR targets that stand in for imported mesh materials, one per alpha mode a glTF
// material carries. An imported material is embedded in its model, so no .material file exists for
// the scan to find; its composed source (and therefore its variant key) depends only on the surface
// and the alpha mode's define, not on its textures or values, so one target per mode cooks for all
// of them: `Rows`, or every kMeshVariants row when empty. Without a mode's target a cooked-only
// runtime has no program for any material in that mode: an alpha-blended glTF material draws
// nothing on the web.
struct ImportedMeshMaterialTarget
{
    const char* Name;
    const char* File;
    const char* DocumentName;
    MaterialAlphaMode AlphaMode;
    std::span<const VariantRequest> Rows;
};
inline const ImportedMeshMaterialTarget kImportedMeshMaterialTargets[] = {
    {"engine-default-pbr", "default_pbr.material", "Default", MaterialAlphaMode::Opaque, {}},
    {"engine-default-pbr-mask", "default_pbr_mask.material", "DefaultMask", MaterialAlphaMode::Mask, {}},
    {"engine-default-pbr-blend", "default_pbr_blend.material", "DefaultBlend", MaterialAlphaMode::Blend, kBlendMeshVariants},
};

inline const VariantRequest kDepthMaskRows[] = {
    {"depth-mask", kDepthMask, VertexAttributeFlags::StandardMesh},
    {"depth-mask+tangent", kDepthMask, VertexAttributeFlags::StandardMeshWithTangent},
    {"depth-mask+skin", kDepthMask, VertexAttributeFlags::SkinnedMesh},
    {"depth-mask+skin+tangent", kDepthMask, VertexAttributeFlags::SkinnedMeshWithTangent},
};

} // namespace GameEngine::Tools::MaterialCook
