#pragma once

namespace GameEngine::Components
{
// Marks a mesh as a DDGI next-event-estimation emitter.
//
// A small bright emissive surface only reaches the probe field when a trace ray
// happens to land on it, which is rarely often enough to converge. Tagging it
// publishes a sphere-proxy light record built from the instance's world bounding
// sphere, so every probe samples it directly, and EXCLUDES the surface from
// per-hit emissive so its energy is never counted twice.
//
// This is a tag rather than a flag on MeshRenderer because "is a GI light" is not
// a property of mesh rendering, and because most meshes are not emitters: the tag
// keeps the byte off every MeshRenderer instance and makes the opt-in an explicit
// authoring gesture rather than a checkbox on a large shared component.
//
// It is deliberately NOT derived from the material's emissive: each emitter costs
// a light record in the trace, so auto-promoting every emissive material would
// turn an authoring convenience into a performance cliff on a scene with many
// emissive surfaces.
//
// @ge-tooltip Publish this mesh's emissive as a DDGI light so probes sample it directly instead of waiting for a ray to hit it. The surface stops contributing emissive on ray hits, so its energy is never counted twice. Costs one DDGI light slot.
struct GIEmitter
{
    // Presence of the component is the signal; this nominal field exists only
    // because the reflection scanner reflects a component only when it has at
    // least one field (same reason RuntimeOnlyEntity carries one).
    bool Active = true;
};

} // namespace GameEngine::Components
