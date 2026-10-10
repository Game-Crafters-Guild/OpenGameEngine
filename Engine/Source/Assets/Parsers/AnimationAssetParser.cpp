#include "Assets/Parsers/AnimationAssetParser.h"

namespace GameEngine
{

// AnimationClip's serializer (AnimationClip::SaveToData) emits version,
// duration, selectedAnimationIndex, source { path, animationIndex }, and
// channels[]. None of those are GUID-form references to other assets that
// belong in the runtime dep graph — channels reference bone names by string
// (not assets); `source.path` records which FBX/glTF the clip was extracted
// from, which is import-time provenance, not a runtime dependency.
//
// Returning true with no edges suppresses the registry's syntactic fallback
// (which would otherwise regex-scan the JSON and emit spurious "Other" edges
// for any GUID-shaped strings it stumbled on). If a future authoring side
// embeds GUID-form references into .anim content (e.g. a humanoid retarget
// rig pin), revisit this method to walk those fields explicitly.
//
// If/when we want producer/sidecar tracking ("which FBX produced this
// clip?"), use the m_AssetProvenance mechanism rather than overloading the
// dep graph — same shape as the model -> material sidecar relationship.
bool AnimationAssetParser::ExtractDependencies([[maybe_unused]] const GUID& referrer,
                                               [[maybe_unused]] const AssetMetadata& metadata,
                                               [[maybe_unused]] DepEdgeSink& sink) const
{
    return true;
}

} // namespace GameEngine
