// Feature-owned provider for the screen-space reflection world-pass targets.
// Everything SSR-specific about the forward MRT lives here — G-buffer slice
// formats, clear semantics, attachment locations, the material keyword, and
// the fallback creation for project rendergraphs that predate the targets —
// so WorldRenderNode and the world pass stay feature-agnostic: they carry a
// generic extra-attachment list and never learn what a slice stores.

#pragma once

#include "Engine/Rendering/RenderServices.h" // WorldPassTargetsRG
#include "Rendering/Materials/ShaderVariantKey.h" // MaterialKeyword

namespace GameEngine::Engine::Renderer::Pipeline
{

struct ViewDeclare;

class ReflectionsProvider
{
public:
    // The keyword this provider will contribute to the world pass for `view`,
    // decided from view state alone: SSSR active, and a colour target whose
    // sample count the single-sample slices can match.
    //
    // Forward contributors that declare BEFORE WorldRenderNode (terrain) must
    // key their variants with this, and must ask for it here rather than read
    // it back from the world pass: RenderServices' recorded world-pass keywords
    // are reset every frame and written by AddWorldPassForView, and the
    // pipeline's frame resource table — where the *Written names land — is
    // rebuilt at graph-build start. Both are therefore empty at the point an
    // earlier node declares, so either probe answers "no SSSR" every frame.
    static Rendering::MaterialKeyword WorldPassKeyword(ViewDeclare& d);

    // Resolve — or create and publish, when the blueprint doesn't declare
    // them — the SSR G-buffer slices for this view, and append them to
    // `targets` as world-pass MRT attachments. Returns the material keyword
    // the world pass must enable so the forward adapter writes the slices,
    // or None when nothing wants them for the view or the color target is
    // multisampled with no consumer target to resolve into (the SSR slices
    // themselves are single-sample).
    // The slices are wanted when SSSR is active, or when a consumer hands in
    // `normalTarget`: its own single-sample render target for the normal
    // slice (a feature that keeps a per-view shading-normal history), which
    // then replaces the blueprint's or the provider's own texture as the
    // attachment — under MSAA as the resolve target of a transient
    // multisampled slice.
    // On the success path this also publishes Names::View::*Written — the
    // positive "slices are attached and written this frame" signal the SSR
    // node requires; every bail path leaves it absent, which disables the
    // node for the view.
    static Rendering::MaterialKeyword ContributeWorldPassTargets(
        ViewDeclare& d, RenderServices::WorldPassTargetsRG& targets,
        Rendering::RenderGraph::RGTexture normalTarget);
};

} // namespace GameEngine::Engine::Renderer::Pipeline
