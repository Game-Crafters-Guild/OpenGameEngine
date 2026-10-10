#pragma once

// PrimitiveGenerator: generates standard 3D primitive meshes (cube, sphere,
// capsule, plane) as GameEngine::Mesh structs suitable for MeshGPURegistry.
//
// Also provides well-known GUIDs for each primitive type so they can be
// looked up by identity (e.g., "give me the sphere") without file I/O.
//
// Responsibilities:
//   - Generate vertex/index data for each primitive type
//   - Provide well-known GUIDs for each primitive
//   - Register all primitives in MeshGPURegistry
//
// Ownership:
//   - Stateless utility (generation functions are static).
//   - GPU resources are owned by MeshGPURegistry after registration.

#include "AssetCore/GUID.h"
#include "Assets/ModelAsset.h" // Mesh, Vertex
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"

#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{

class RenderServices;

class PrimitiveGenerator
{
  public:
    // Well-known GUIDs for default primitives and built-in materials.
    // These are deterministic (derived from Null GUID) and never change.
    static GUID CubeGuid();
    static GUID SphereGuid();
    static GUID CapsuleGuid();
    static GUID PlaneGuid();
    /// Same XZ quad as Plane with normal +Y, but with sprite-oriented UVs so
    /// the first image row (PNG top-of-file via STBi) aligns with billboard
    /// "screen up" after rotating the quad for 2D cameras (fixes upside-down
    /// sprites without double-sided materials).
    static GUID PlaneSpriteUvGuid();
    static GUID DefaultMaterialGuid();
    static GUID ReflectionProbeTestMaterialGuid();

    // Generate primitive meshes. These return fully-populated Mesh structs
    // (with position, normal, UV) ready for MeshGPURegistry upload.
    static Mesh GenerateCube(float halfExtent = 0.5f);
    static Mesh GenerateSphere(float radius = 0.5f, uint32_t segments = 64, uint32_t rings = 32);
    static Mesh GenerateCapsule(float radius = 0.5f, float halfHeight = 0.5f, uint32_t segments = 32, uint32_t rings = 8);
    static Mesh GeneratePlane(float halfExtent = 0.5f);
    /// GeneratePlane geometry with sprite-oriented UV coordinates (same geometry
    /// and winding).
    static Mesh GeneratePlaneSpriteUv(float halfExtent = 0.5f);

    /// Generate the primitive that a well-known GUID names, at its default size.
    /// Returns an empty Mesh when the GUID is not a primitive.
    ///
    /// This is the single GUID -> geometry table. `RegisterAll` builds the registry
    /// from it and device-rebuild recovery regenerates from it, so a primitive added
    /// here is both registered and recovered. One added to a caller's own list would
    /// be registered and then silently lost on the next device loss.
    static Mesh GenerateByGuid(const GUID& guid);

    // Register all default primitives in MeshGPURegistry.
    // After this call, primitives are available via:
    //   MeshGPURegistry::FindByKey({PrimitiveGenerator::CubeGuid(), 0})
    static void RegisterAll(RenderServices& rs);

    // Resolve a primitive name ("Cube", "Sphere", "Capsule", "Plane") to its GUID.
    // Returns null GUID if the name is not a known primitive.
    static GUID GuidFromName(std::string_view name);

    // Resolve a GUID to its primitive name. Returns empty string if not a primitive.
    static std::string_view NameFromGuid(const GUID& guid);

    // Resolve a GUID to a built-in MATERIAL name, or empty when it is not one.
    // Deliberately separate from NameFromGuid: that answers "is this a primitive
    // MESH" and IsPrimitive() is built on it, so folding materials in there would
    // make a material GUID claim to be a mesh.
    static std::string_view MaterialNameFromGuid(const GUID& guid);

    // Check if a GUID is a known primitive.
    static bool IsPrimitive(const GUID& guid);

    // Create a fully populated MeshRenderer component for a primitive mesh.
    // Sets meshGpuHandleId (if rs available), modelAssetGuid, and materialAssetGuid.
    // Use this everywhere primitives are created to ensure consistent serialization.
    static Components::MeshRenderer MakePrimitiveMeshRenderer(
        RenderServices* rs, const GUID& meshGuid, const GUID& materialGuid);

    // Create a LocalBounds component matching the primitive's mesh geometry.
    //
    // When `rs` is non-null AND the primitive has been registered (via
    // `RegisterAll` or a custom `RegisterSubmesh` call), the bounds are
    // read directly from the registered mesh's MeshGPUEntry — so a
    // caller who registered a non-default-sized primitive (e.g.
    // `RegisterSubmesh({CapsuleGuid, 0}, GenerateCapsule(2.0f, 1.5f))`)
    // gets matching LocalBounds without the factory needing to know
    // those parameters.
    //
    // When `rs` is null OR the GUID isn't registered, falls back to the
    // default-parameter geometry for the GUID:
    //   Cube/Sphere: unit cube (halfExtents 0.5,0.5,0.5)
    //   Capsule:     1×2×1 box (halfExtents 0.5,1.0,0.5) — cylinder portion +
    //                hemisphere caps along Y at radius=0.5, halfHeight=0.5.
    //   Plane:       1×0×1 box (halfExtents 0.5,0,0.5) — flat in XZ.
    //   Unknown:     unit cube as a defensive fallback.
    //
    // Use this everywhere primitives are spawned so they carry real
    // bounds for picking, culling, and any other spatial query — instead
    // of relying on per-system fallback rules.
    static Components::LocalBounds MakePrimitiveLocalBounds(
        RenderServices* rs, const GUID& meshGuid);

    // Convenience overload for legacy callers without RenderServices.
    // Equivalent to MakePrimitiveLocalBounds(nullptr, meshGuid) — falls
    // back to default-parameter geometry without consulting the registry.
    static Components::LocalBounds MakePrimitiveLocalBounds(const GUID& meshGuid)
    {
        return MakePrimitiveLocalBounds(nullptr, meshGuid);
    }
};

} // namespace Engine::Renderer
} // namespace GameEngine
