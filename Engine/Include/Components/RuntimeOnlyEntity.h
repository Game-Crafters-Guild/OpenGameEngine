#pragma once

namespace GameEngine::Components
{
// Tag marking an entity as engine-created runtime state that must never be written to a
// scene file. SaveSceneToFile excludes any entity carrying this tag, the same way it
// excludes blueprint-instanced entities, so the entity exists only for the duration of a
// session and is reconstructed at runtime on the next load. The first consumer is the HLOD
// proxy entity, spawned from a baked .gehlod cluster; without this exclusion those proxies
// would serialize into the user's scene and double-spawn (with dangling synthetic mesh
// GUIDs) on reload.
//
// The tag is intentionally kept out of the Add Component menu — a user adding it by hand
// would silently drop the entity from every save — and is itself excluded from
// serialization (belt-and-braces: the entity-level skip already keeps it out of any file).
//
// @ge-no-add        applied by engine subsystems, never added by hand in the editor
// [DoNotSerialize]  the tag is runtime state; it must not appear in a saved scene
struct RuntimeOnlyEntity
{
    // A marker of generator output, not a feature: it has no off state.
    static constexpr bool NotToggleable = true;

    // Presence of the component is the signal; this nominal flag exists only because the
    // reflection scanner reflects (and thus DoNotSerialize-marks) a component only when it
    // has at least one field.
    bool Active = true;
};

} // namespace GameEngine::Components
