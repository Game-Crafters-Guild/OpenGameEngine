#pragma once

// Per-(feature, view) declaration scope for IRenderFeature::Declare. Built fresh
// each frame by RenderServices::MakeFeatureDeclareContext; nothing here is stored
// across frames. Everything is a DATA SNAPSHOT resolved once (frame-validated) so
// a feature never reads RenderServices' private per-frame state during a builder
// lambda — plus a PassName callable and a ShadowDeclareSeam capability token.
//
// The feature reaches the shared RenderServices plumbing through the
// RenderServices& that Declare receives (see IRenderFeature::Declare): it calls
// the passkey-gated methods (rs.ImportShadowMapArrayRG / ImportTransmittanceShadowArrayRG
// / BuildShadowPassResources) with ctx.Seam, and records via the free
// RecordDepthOnlyPass over rs.MakeDepthDrawServices() directly from the pass exec
// lambda. There is no back-door RenderServices& on the
// ctx and no friend grant on RenderServices — only the enumerated seam below.

#include "Engine/Rendering/ShadowFrameInfo.h"  // Area/Spot/PointShadowFrameInfo snapshots
#include "Rendering/CameraTypes.h"              // ViewId
#include "Rendering/Core/RenderGraph/RGFrame.h" // RGBuffer

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
class RenderServices;
struct FeatureDeclareContext;

// Capability token gating the RenderServices shadow-declaration plumbing
// (ImportShadowMapArrayRG / ImportTransmittanceShadowArrayRG /
// BuildShadowPassResources / the S2 ViewFrameRG publish setters). It carries no
// state; it exists only so those RenderServices methods can require a caller who
// holds a token that ONLY a FeatureDeclareContext can carry — and a
// FeatureDeclareContext can only be minted by RenderServices. This is the
// sanctioned narrow alternative to `friend class ShadowMapRenderFeature` on
// RenderServices: the friend grants live on these two tiny types, never on RS.
class ShadowDeclareSeam
{
  public:
    ShadowDeclareSeam(const ShadowDeclareSeam&) = default;
    ShadowDeclareSeam& operator=(const ShadowDeclareSeam&) = default;

  private:
    ShadowDeclareSeam() = default;
    friend struct FeatureDeclareContext; // the only minter
};

struct FeatureDeclareContext
{
    // Copyable/movable so RenderServices::MakeFeatureDeclareContext can return it
    // by value; only the default ctor is restricted (RS mints the snapshot).
    FeatureDeclareContext(const FeatureDeclareContext&) = default;
    FeatureDeclareContext(FeatureDeclareContext&&) = default;
    FeatureDeclareContext& operator=(const FeatureDeclareContext&) = default;
    FeatureDeclareContext& operator=(FeatureDeclareContext&&) = default;

    // ---- node config that parameterizes the shadow families ----
    Rendering::ViewId View = 0;
    uint32_t CascadeCount = 0;       // directional cascade count (CSM node's m_NumCascades)
    uint32_t PunctualResolution = 0; // area/spot/point shadow-map resolution

    // First shadow-casting directional light's world direction, or nullptr when
    // the view has none (the node's no-light branch): present => Declare emits
    // cascades + tint THEN the punctual families; null => Declare emits the
    // punctual families only (the no-light branch). Points into the caller's
    // ExtractedLight; valid only for the synchronous Declare call.
    const float* DirectionalLightDirWS = nullptr;

    // Frame-graph spine values the producer builder lambdas Read to re-derive the
    // skinning->pass and bucketer->pass RAW edges. ALREADY frame-validated
    // (RenderServices applies the m_FrameRG.IsFor guard when snapshotting): these
    // are invalid when the spine did not publish them into THIS frame, so the
    // feature's `if (SkinPaletteAtlas.IsValid()) p.Read(SkinPaletteAtlas)` matches
    // the pre-move `if (m_FrameRG.IsFor(...) && m_FrameRG.X.IsValid())` exactly.
    Rendering::RenderGraph::RGBuffer SkinPaletteAtlas{};
    Rendering::RenderGraph::RGBuffer DrawStreamOrdering{};
    // The loud else-branch diagnostics (mirror the current Log::Error arms):
    // pending work but no published spine value = out-of-order declaration.
    bool HasPendingSkinnedInstances = false;
    bool HasPendingBucketerSlices = false;

    // The WAR misorder tripwire snapshot (ViewFrameRG.WorldDeclared): a producer
    // declared after the view's world pass silently samples frame N-1.
    bool WorldAlreadyDeclared = false;

    // Declaration-time activation predicates, snapshotted once (per-view, stable
    // across a single Declare — nothing between the two loops mutates them).
    bool ViewNeedsCascades = false;
    // Tint activation predicate: transmissive geometry that CASTS shadows. Visible
    // glass alone is not enough — a non-casting instance is dropped by the shadow
    // cull, so a tint cascade declared for it can only ever clear to the value the
    // 1x1 fallback already carries.
    bool HasTransmissiveCaster = false;

    // Punctual (area/spot/point) shadow geometry, resolved ONCE by
    // MakeFeatureDeclareContext (FindViewDesc -> GetWorldLights -> Build*ShadowFrameInfo)
    // and present only when the view is active, has shadow casters, and a valid
    // light of that family exists — i.e. the exact old
    // ViewNeeds{Area,Spot,Point}ShadowPasses gate collapsed into has_value().
    // The old RenderServices bodies double-evaluated this (the ViewNeeds predicate
    // AND a second Build* inside the body); the snapshot removes that double-eval,
    // so the feature's DeclarePass reads `if (!ctx.AreaShadow) return {}` off one
    // resolved value. The publish back to ViewFrameRG is write-only (dedup branch
    // always-taken within one declaration; ViewFrameRG is frame-local).
    std::optional<AreaShadowFrameInfo> AreaShadow;
    std::optional<SpotShadowFrameInfo> SpotShadow;
    // M1: the budgeted, importance-ranked set of shadow-casting point lights
    // (was a single std::optional). Each entry carries its assigned atlas slot
    // (PointShadowFrameInfo::shadowSlot) and committed tile resolution; the set is
    // dense (only admitted slots) but slot indices may be sparse across the atlas.
    // Empty when the view has no shadowed point light. PointShadowBudget sizes the
    // atlas (budget * 6 layers) independent of how many slots are occupied.
    std::vector<PointShadowFrameInfo> PointShadows;
    uint32_t PointShadowBudget = 0;

    // Stable framework pass name (MCP + perf-CSV name contracts). Bound by the
    // node to [&d](const char* s){ return d.PassName(s); }; the feature's Declare
    // builds per-cascade suffixes ("Cascade%u" / "GlassTint%u") through it. Empty
    // in granular tests that pass a literal pass name to the per-family methods.
    std::function<std::string(const char*)> PassName;

    // Capability token for the passkey-gated RenderServices plumbing (above).
    // Minted by RenderServices when it builds the ctx; copied out by the feature
    // to pass to the gated methods.
    ShadowDeclareSeam Seam;

  private:
    FeatureDeclareContext() = default;    // only RenderServices mints a ctx
    friend class RenderServices;
};

} // namespace Engine::Renderer
} // namespace GameEngine
