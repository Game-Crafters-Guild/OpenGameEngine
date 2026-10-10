#pragma once

namespace GameEngine::Engine::Renderer
{

// Binds DDGIVolume's inspector slider bounds once, after the build-time
// scanner's generated reflection has registered the component's field table
// at static init (see Engine/Modules/Ocean/Source/OceanFieldRanges.cpp for
// the identical pattern and why this must be an explicitly-called function
// rather than a namespace-scope static: SetReflectedFieldRange requires the
// target field table to already exist, and static-initialization order
// across translation units — this file's vs. the scanner-generated one — is
// unspecified, so the call must be deferred to a point in engine startup
// guaranteed to run after all TU statics, not tied to this TU's own static
// init). Idempotent; safe to call more than once.
void RegisterDDGIFieldRanges();

}  // namespace GameEngine::Engine::Renderer
