// PrimitiveGenerator implementation: generates standard 3D primitives and
// registers them through MeshGPURegistry with well-known GUIDs.

#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Assets/ModelAsset.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Materials/MaterialDocument.h"


#include <array>
#include <cmath>
#include <cstring>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{

constexpr float kPi = 3.14159265358979323846f;

Vertex MakeVertex(float px, float py, float pz,
                  float nx, float ny, float nz,
                  float u, float v)
{
    Vertex out{};
    out.Position[0] = px; out.Position[1] = py; out.Position[2] = pz;
    out.Normal[0] = nx;   out.Normal[1] = ny;   out.Normal[2] = nz;
    out.TexCoords[0] = u; out.TexCoords[1] = v;
    return out;
}

void ComputeMeshBounds(Mesh& mesh)
{
    if (mesh.Vertices.empty()) return;
    float minB[3] = {1e30f, 1e30f, 1e30f};
    float maxB[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& v : mesh.Vertices)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (v.Position[i] < minB[i]) minB[i] = v.Position[i];
            if (v.Position[i] > maxB[i]) maxB[i] = v.Position[i];
        }
    }
    std::memcpy(mesh.MinBounds, minB, sizeof(minB));
    std::memcpy(mesh.MaxBounds, maxB, sizeof(maxB));
}

void FinalizePrimitiveMesh(Mesh& mesh)
{
    GenerateMeshTangents(mesh);
    ComputeMeshBounds(mesh);
}

} // namespace

// --- Well-known GUIDs ---

GUID PrimitiveGenerator::CubeGuid()            { return GUID::Derive(GUID::Null(), "engine/primitive/cube"); }
GUID PrimitiveGenerator::SphereGuid()          { return GUID::Derive(GUID::Null(), "engine/primitive/sphere"); }
GUID PrimitiveGenerator::CapsuleGuid()         { return GUID::Derive(GUID::Null(), "engine/primitive/capsule"); }
GUID PrimitiveGenerator::PlaneGuid()           { return GUID::Derive(GUID::Null(), "engine/primitive/plane"); }
GUID PrimitiveGenerator::PlaneSpriteUvGuid()   { return GUID::Derive(GUID::Null(), "engine/primitive/plane_sprite_uv"); }
GUID PrimitiveGenerator::DefaultMaterialGuid() { return GUID::Derive(GUID::Null(), "engine/material/default_unlit"); }
GUID PrimitiveGenerator::ReflectionProbeTestMaterialGuid()
{
    return GUID::Derive(GUID::Null(), "engine/material/reflection_probe_test");
}

GUID PrimitiveGenerator::GuidFromName(std::string_view name)
{
    // Case-insensitive comparison for robustness.
    auto eq = [](std::string_view a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    if (eq(name, "cube"))    return CubeGuid();
    if (eq(name, "sphere"))  return SphereGuid();
    if (eq(name, "capsule")) return CapsuleGuid();
    if (eq(name, "plane"))           return PlaneGuid();
    if (eq(name, "planespriteuv"))   return PlaneSpriteUvGuid();
    if (eq(name, "plane_sprite"))    return PlaneSpriteUvGuid();
    if (eq(name, "plane_sprite_uv")) return PlaneSpriteUvGuid();
    return GUID::Null();
}

std::string_view PrimitiveGenerator::NameFromGuid(const GUID& guid)
{
    if (guid == CubeGuid())    return "Cube";
    if (guid == SphereGuid())  return "Sphere";
    if (guid == CapsuleGuid()) return "Capsule";
    if (guid == PlaneGuid())        return "Plane";
    if (guid == PlaneSpriteUvGuid()) return "PlaneSpriteUv";
    return {};
}

std::string_view PrimitiveGenerator::MaterialNameFromGuid(const GUID& guid)
{
    if (guid == DefaultMaterialGuid())             return "Default Material";
    if (guid == ReflectionProbeTestMaterialGuid()) return "Reflection Probe Test Material";
    return {};
}

bool PrimitiveGenerator::IsPrimitive(const GUID& guid)
{
    return !NameFromGuid(guid).empty();
}

Components::MeshRenderer PrimitiveGenerator::MakePrimitiveMeshRenderer(
    RenderServices* rs, const GUID& meshGuid, const GUID& materialGuid)
{
    Components::MeshRenderer mr{};
    if (rs)
    {
        auto meshHandle = rs->GetMeshGPURegistry().FindHandle(
            Rendering::MeshGPUKey{meshGuid, 0});
        mr.meshGpuHandleId = meshHandle.IsValid() ? static_cast<uint64>(meshHandle) : 0;
    }
    // Store GUIDs for portable scene serialization.
    mr.modelAssetGuid.Set(meshGuid);
    mr.materialAssetGuid.Set(materialGuid);
    mr.renderLayerMask = 1u;
    return mr;
}

Components::LocalBounds PrimitiveGenerator::MakePrimitiveLocalBounds(
    RenderServices* rs, const GUID& meshGuid)
{
    // Fast path: if a registered MeshGPUEntry exists for this GUID, its
    // bounds are derived from the actual generated geometry (whatever
    // params were used at GenerateXxx-time). Use those directly so a
    // caller who registered a non-default-sized primitive gets correct
    // bounds.
    if (rs)
    {
        const auto* entry = rs->GetMeshGPURegistry().FindByKey(
            Rendering::MeshGPUKey{meshGuid, 0u});
        if (entry)
        {
            Components::LocalBounds lb{};
            lb.Box = entry->bounds;
            return lb;
        }
    }

    // Fallback: default-parameter geometry for the GUID. Matches the
    // bounds RegisterAll would produce (since RegisterAll calls each
    // GenerateXxx with no args, which in turn uses defaults), so the
    // fallback path is identical to the registry-lookup path for
    // default-parameter primitives. Diverges only if a caller
    // explicitly registered with non-default params AND skipped passing
    // rs to this factory.
    Components::LocalBounds lb{};
    lb.Box.center = {0.0f, 0.0f, 0.0f};

    if (meshGuid == CapsuleGuid())
    {
        // Default capsule: radius 0.5, halfHeight 0.5 → total height 2.0
        // (cylinder length 1.0 + 2 hemisphere caps of radius 0.5).
        lb.Box.halfExtents = {0.5f, 1.0f, 0.5f};
    }
    else if (meshGuid == PlaneGuid() || meshGuid == PlaneSpriteUvGuid())
    {
        // Plane is flat in XZ; zero Y extent. The slab test handles the
        // degenerate axis correctly (tMin == tMax on Y when origin.y is
        // exactly on the plane), so a zero-height AABB is fine for picking.
        lb.Box.halfExtents = {0.5f, 0.0f, 0.5f};
    }
    else
    {
        // Cube, Sphere, and any unknown GUID default to the unit cube
        // (halfExtents 0.5,0.5,0.5) — matches the geometry of both the
        // default cube primitive and the default sphere primitive
        // (radius 0.5 inscribes a unit cube).
        lb.Box.halfExtents = {0.5f, 0.5f, 0.5f};
    }
    return lb;
}

// --- Cube ---

Mesh PrimitiveGenerator::GenerateCube(float h)
{
    Mesh mesh{};
    mesh.Name = "Cube";
    mesh.Vertices.reserve(24);
    mesh.Indices.reserve(36);
    mesh.MaterialIndex = 0;

    // 6 faces, 4 verts each, unique normals per face.
    struct FaceData { float nx, ny, nz; float v[4][3]; };
    const FaceData faces[] = {
        {+1, 0, 0, {{+h,-h,-h}, {+h,-h,+h}, {+h,+h,+h}, {+h,+h,-h}}},
        {-1, 0, 0, {{-h,-h,+h}, {-h,-h,-h}, {-h,+h,-h}, {-h,+h,+h}}},
        { 0,+1, 0, {{-h,+h,-h}, {+h,+h,-h}, {+h,+h,+h}, {-h,+h,+h}}},
        { 0,-1, 0, {{-h,-h,+h}, {+h,-h,+h}, {+h,-h,-h}, {-h,-h,-h}}},
        { 0, 0,+1, {{+h,-h,+h}, {-h,-h,+h}, {-h,+h,+h}, {+h,+h,+h}}},
        { 0, 0,-1, {{-h,-h,-h}, {+h,-h,-h}, {+h,+h,-h}, {-h,+h,-h}}},
    };
    const float uvs[][2] = {{0,0},{1,0},{1,1},{0,1}};

    for (const auto& f : faces)
    {
        uint32_t base = static_cast<uint32_t>(mesh.Vertices.size());
        for (int i = 0; i < 4; ++i)
            mesh.Vertices.push_back(MakeVertex(f.v[i][0], f.v[i][1], f.v[i][2], f.nx, f.ny, f.nz, uvs[i][0], uvs[i][1]));
        mesh.Indices.push_back(base+0); mesh.Indices.push_back(base+2); mesh.Indices.push_back(base+1);
        mesh.Indices.push_back(base+0); mesh.Indices.push_back(base+3); mesh.Indices.push_back(base+2);
    }

    FinalizePrimitiveMesh(mesh);
    return mesh;
}

// --- Plane ---

Mesh PrimitiveGenerator::GeneratePlane(float h)
{
    Mesh mesh{};
    mesh.Name = "Plane";
    mesh.MaterialIndex = 0;

    mesh.Vertices.push_back(MakeVertex(-h, 0, -h, 0, 1, 0, 1, 0));
    mesh.Vertices.push_back(MakeVertex(+h, 0, -h, 0, 1, 0, 0, 0));
    mesh.Vertices.push_back(MakeVertex(+h, 0, +h, 0, 1, 0, 0, 1));
    mesh.Vertices.push_back(MakeVertex(-h, 0, +h, 0, 1, 0, 1, 1));

    mesh.Indices = {0, 2, 1, 0, 3, 2};

    FinalizePrimitiveMesh(mesh);
    return mesh;
}

Mesh PrimitiveGenerator::GeneratePlaneSpriteUv(float h)
{
    Mesh mesh{};
    mesh.Name = "PlaneSpriteUv";
    mesh.MaterialIndex = 0;

    // Same positions/indices/winding as GeneratePlane; UVs stay sprite-oriented
    // so the first decoded image row (PNG top) matches billboard "up" for the
    // Sprite plane roll (+90° X when using CreateSpriteEntityFromTexture 2D mode)
    // for 2D cameras that look along +Z.
    mesh.Vertices.push_back(MakeVertex(-h, 0, -h, 0, 1, 0, 0, 1));
    mesh.Vertices.push_back(MakeVertex(+h, 0, -h, 0, 1, 0, 1, 1));
    mesh.Vertices.push_back(MakeVertex(+h, 0, +h, 0, 1, 0, 1, 0));
    mesh.Vertices.push_back(MakeVertex(-h, 0, +h, 0, 1, 0, 0, 0));

    mesh.Indices = {0, 2, 1, 0, 3, 2};

    FinalizePrimitiveMesh(mesh);
    return mesh;
}

// --- Sphere ---

Mesh PrimitiveGenerator::GenerateSphere(float radius, uint32_t segments, uint32_t rings)
{
    Mesh mesh{};
    mesh.Name = "Sphere";
    mesh.MaterialIndex = 0;

    for (uint32_t y = 0; y <= rings; ++y)
    {
        float phi = kPi * static_cast<float>(y) / static_cast<float>(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (uint32_t x = 0; x <= segments; ++x)
        {
            float theta = 2.0f * kPi * static_cast<float>(x) / static_cast<float>(segments);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            float nx = sinPhi * cosTheta;
            float ny = cosPhi;
            float nz = sinPhi * sinTheta;

            float u = static_cast<float>(x) / static_cast<float>(segments);
            float v = static_cast<float>(y) / static_cast<float>(rings);

            mesh.Vertices.push_back(MakeVertex(nx * radius, ny * radius, nz * radius, nx, ny, nz, u, v));
        }
    }

    for (uint32_t y = 0; y < rings; ++y)
    {
        for (uint32_t x = 0; x < segments; ++x)
        {
            uint32_t a = y * (segments + 1) + x;
            uint32_t b = a + segments + 1;
            mesh.Indices.push_back(a);     mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b);
            mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b + 1); mesh.Indices.push_back(b);
        }
    }

    FinalizePrimitiveMesh(mesh);
    return mesh;
}

// --- Capsule ---

Mesh PrimitiveGenerator::GenerateCapsule(float radius, float halfHeight, uint32_t segments, uint32_t rings)
{
    Mesh mesh{};
    mesh.Name = "Capsule";
    mesh.MaterialIndex = 0;

    // Top hemisphere
    for (uint32_t y = 0; y <= rings; ++y)
    {
        float phi = (kPi * 0.5f) * static_cast<float>(y) / static_cast<float>(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (uint32_t x = 0; x <= segments; ++x)
        {
            float theta = 2.0f * kPi * static_cast<float>(x) / static_cast<float>(segments);
            float nx = sinPhi * std::cos(theta);
            float ny = cosPhi;
            float nz = sinPhi * std::sin(theta);

            float u = static_cast<float>(x) / static_cast<float>(segments);
            float v = static_cast<float>(y) / static_cast<float>(rings * 2 + 1);

            mesh.Vertices.push_back(MakeVertex(nx * radius, ny * radius + halfHeight, nz * radius, nx, ny, nz, u, v));
        }
    }

    // Cylinder body (two rings)
    for (int side = 0; side < 2; ++side)
    {
        float yOffset = (side == 0) ? halfHeight : -halfHeight;
        float vBase = (side == 0) ? 0.5f - 0.1f : 0.5f + 0.1f;

        for (uint32_t x = 0; x <= segments; ++x)
        {
            float theta = 2.0f * kPi * static_cast<float>(x) / static_cast<float>(segments);
            float nx = std::cos(theta);
            float nz = std::sin(theta);
            float u = static_cast<float>(x) / static_cast<float>(segments);

            mesh.Vertices.push_back(MakeVertex(nx * radius, yOffset, nz * radius, nx, 0, nz, u, vBase));
        }
    }

    // Bottom hemisphere
    for (uint32_t y = 0; y <= rings; ++y)
    {
        float phi = (kPi * 0.5f) + (kPi * 0.5f) * static_cast<float>(y) / static_cast<float>(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (uint32_t x = 0; x <= segments; ++x)
        {
            float theta = 2.0f * kPi * static_cast<float>(x) / static_cast<float>(segments);
            float nx = sinPhi * std::cos(theta);
            float ny = cosPhi;
            float nz = sinPhi * std::sin(theta);

            float u = static_cast<float>(x) / static_cast<float>(segments);
            float v = 0.5f + 0.5f * static_cast<float>(y) / static_cast<float>(rings);

            mesh.Vertices.push_back(MakeVertex(nx * radius, ny * radius - halfHeight, nz * radius, nx, ny, nz, u, v));
        }
    }

    // Indices: top hemisphere
    uint32_t stride = segments + 1;
    for (uint32_t y = 0; y < rings; ++y)
    {
        for (uint32_t x = 0; x < segments; ++x)
        {
            uint32_t a = y * stride + x;
            uint32_t b = a + stride;
            mesh.Indices.push_back(a);     mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b);
            mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b + 1); mesh.Indices.push_back(b);
        }
    }

    // Indices: cylinder body (connect top hemisphere bottom ring to cylinder top, cylinder to bottom hemisphere)
    uint32_t topHemiEnd = (rings + 1) * stride;
    uint32_t cylTop = topHemiEnd;
    uint32_t cylBot = cylTop + stride;
    uint32_t botHemiStart = cylBot + stride;

    // Top hemi bottom -> cylinder top
    {
        uint32_t hemiRow = rings * stride;
        for (uint32_t x = 0; x < segments; ++x)
        {
            uint32_t a = hemiRow + x;
            uint32_t b = cylTop + x;
            mesh.Indices.push_back(a);     mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b);
            mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b + 1); mesh.Indices.push_back(b);
        }
    }

    // Cylinder top -> cylinder bottom
    for (uint32_t x = 0; x < segments; ++x)
    {
        uint32_t a = cylTop + x;
        uint32_t b = cylBot + x;
        mesh.Indices.push_back(a);     mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b);
        mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b + 1); mesh.Indices.push_back(b);
    }

    // Cylinder bottom -> bottom hemisphere top
    for (uint32_t x = 0; x < segments; ++x)
    {
        uint32_t a = cylBot + x;
        uint32_t b = botHemiStart + x;
        mesh.Indices.push_back(a);     mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b);
        mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b + 1); mesh.Indices.push_back(b);
    }

    // Bottom hemisphere
    for (uint32_t y = 0; y < rings; ++y)
    {
        for (uint32_t x = 0; x < segments; ++x)
        {
            uint32_t a = botHemiStart + y * stride + x;
            uint32_t b = a + stride;
            mesh.Indices.push_back(a);     mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b);
            mesh.Indices.push_back(a + 1); mesh.Indices.push_back(b + 1); mesh.Indices.push_back(b);
        }
    }

    FinalizePrimitiveMesh(mesh);
    return mesh;
}

// --- Generation by identity ---

namespace
{
// The GUID -> generator table. Every consumer of "which primitives exist" reads
// this one array, so registration and device-rebuild recovery cannot disagree.
struct PrimitiveDef
{
    GUID (*Guid)();
    Mesh (*Generate)();
};

const PrimitiveDef kPrimitiveDefs[] = {
    {&PrimitiveGenerator::CubeGuid,          [] { return PrimitiveGenerator::GenerateCube(); }},
    {&PrimitiveGenerator::SphereGuid,        [] { return PrimitiveGenerator::GenerateSphere(); }},
    {&PrimitiveGenerator::CapsuleGuid,       [] { return PrimitiveGenerator::GenerateCapsule(); }},
    {&PrimitiveGenerator::PlaneGuid,         [] { return PrimitiveGenerator::GeneratePlane(); }},
    {&PrimitiveGenerator::PlaneSpriteUvGuid, [] { return PrimitiveGenerator::GeneratePlaneSpriteUv(); }},
};
} // namespace

Mesh PrimitiveGenerator::GenerateByGuid(const GUID& guid)
{
    // GUID::Derive hashes a string, and device-rebuild recovery consults this for
    // every entry with no resident asset — resolve the table's identities once.
    static const std::array<GUID, std::size(kPrimitiveDefs)> kGuids = [] {
        std::array<GUID, std::size(kPrimitiveDefs)> guids{};
        for (size_t i = 0; i < std::size(kPrimitiveDefs); ++i)
            guids[i] = kPrimitiveDefs[i].Guid();
        return guids;
    }();

    for (size_t i = 0; i < kGuids.size(); ++i)
    {
        if (kGuids[i] == guid)
            return kPrimitiveDefs[i].Generate();
    }
    return Mesh{};
}

// --- RegisterAll ---

void PrimitiveGenerator::RegisterAll(RenderServices& rs)
{
    auto& meshReg = rs.GetMeshGPURegistry();

    for (const PrimitiveDef& def : kPrimitiveDefs)
    {
        Rendering::MeshGPUKey key{def.Guid(), 0};
        Mesh mesh = def.Generate();
        // Retain the CPU copy: these are generated meshes with no ModelAsset
        // behind them, so a consumer that reads geometry back on the CPU can
        // reach them ONLY through the registry's own mirror. Without it the
        // DDGI software lane builds a BVH containing no primitives at all —
        // a scene built from cubes and planes then produces zero GI, silently,
        // because that lane omits meshes it cannot resolve rather than failing.
        // Five small meshes, a few KB total.
        meshReg.RegisterSubmesh(key, mesh, /*retainCpuMesh=*/true);
    }

    // Register a default PBR material for primitives and fallback usage.
    // Uses standard_pbr (same as glTF models) so shader variant caches are
    // shared — primitives pre-warm the variants that models need.
    if (!rs.Materials().Registry().Find(DefaultMaterialGuid()))
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Default");
        rs.RegisterAndPrewarmMaterial(DefaultMaterialGuid(), doc);
    }

    if (!rs.Materials().Registry().Find(ReflectionProbeTestMaterialGuid()))
    {
        MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Reflection Probe Test");
        doc.properties["baseColor"] = std::vector<float>{0.82f, 0.9f, 1.0f, 1.0f};
        doc.properties["metallic"] = 1.0f;
        doc.properties["roughness"] = 0.04f;
        rs.RegisterAndPrewarmMaterial(ReflectionProbeTestMaterialGuid(), doc);
    }
}

} // namespace Engine::Renderer
} // namespace GameEngine
