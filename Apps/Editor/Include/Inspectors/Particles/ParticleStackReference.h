#pragma once

namespace GameEngine
{
class AssetManager;
class GUID;
} // namespace GameEngine

namespace GameEngine::ParticleInspectors
{

/// What an emitter's stack reference resolves to, for the inspector to say.
enum class ParticleStackReference
{
    /// No reference: the emitter runs the default stack.
    Default,
    /// The stack asset is loaded.
    Loaded,
    /// No asset carries the GUID; the emitter emits nothing.
    Missing,
    /// The asset exists and is not loaded yet.
    Loading,
    /// The GUID names a loaded asset of another type; the emitter emits nothing.
    NotAStack,
};

/// Resolves `stack` against `assets`.
ParticleStackReference ResolveParticleStackReference(const AssetManager& assets, const GUID& stack);

} // namespace GameEngine::ParticleInspectors
