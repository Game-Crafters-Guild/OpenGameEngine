#include "Assets/PolyhavenPlaceholderFactory.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "Components/PolyhavenPlaceholder.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "AssetCore/GUID.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace GameEngine
{

// Read PNG width and height from the file header (IHDR chunk).
// Returns false if the file can't be read or isn't a valid PNG.
static bool ReadPngDimensions(const std::filesystem::path& path, uint32_t& outW, uint32_t& outH)
{
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
        return false;
    // PNG header (8 bytes) + IHDR length (4) + IHDR type (4) + width (4) + height (4) = 24 bytes
    uint8_t buf[24];
    bool ok = (std::fread(buf, 1, 24, f) == 24);
    std::fclose(f);
    if (!ok)
        return false;
    // Verify PNG signature.
    static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(buf, kPngSig, 8) != 0)
        return false;
    // Width and height are big-endian uint32 at offsets 16 and 20.
    outW = (uint32_t(buf[16]) << 24) | (uint32_t(buf[17]) << 16) | (uint32_t(buf[18]) << 8) | buf[19];
    outH = (uint32_t(buf[20]) << 24) | (uint32_t(buf[21]) << 16) | (uint32_t(buf[22]) << 8) | buf[23];
    return outW > 0 && outH > 0;
}

// Cylindrical billboard: rotate the plane around world-Y so its normal
// faces the camera horizontally, keeping the plane upright.
// The generated plane lies in XZ with Y+ as its normal.  After rotation:
//   col0 (local X) = camera-right       →  spans the plane width
//   col1 (local Y) = toward camera (XZ) →  plane normal
//   col2 (local Z) = world down         →  spans the plane height (flipped so UV v=0 is top)
// Columns are scaled by scaleX/scaleY to preserve the thumbnail aspect ratio.
static void OrientBillboard(Components::Transform& xf,
                            const Mathematics::Vector3& worldPos,
                            const Mathematics::Vector3& cameraPos,
                            float scaleX = 1.0f, float scaleY = 1.0f)
{
    float dx = cameraPos.x - worldPos.x;
    float dz = cameraPos.z - worldPos.z;
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-6f)
        return; // camera directly above/below — keep identity
    dx /= len;
    dz /= len;

    // col0 = camera-right = (dz, 0, -dx)   (u increases left-to-right from camera)
    // col1 = toward camera in XZ = (dx, 0, dz)
    // col2 = down = (0, -1, 0)             (v=0 at top, v=1 at bottom → image right-side-up)
    xf.matrix[0]  =  dz * scaleX; xf.matrix[1]  = 0.0f;     xf.matrix[2]  = -dx * scaleX; xf.matrix[3]  = 0.0f;
    xf.matrix[4]  =  dx;          xf.matrix[5]  = 0.0f;     xf.matrix[6]  =  dz;          xf.matrix[7]  = 0.0f;
    xf.matrix[8]  =  0.0f;        xf.matrix[9]  = -scaleY;  xf.matrix[10] =  0.0f;        xf.matrix[11] = 0.0f;
}

// Dedicated plane mesh for billboards — same geometry as PlaneGuid() but a
// separate GUID so the billboard never shares mesh identity with the default
// scene ground plane.
static GUID GetOrRegisterBillboardMesh(Engine::Renderer::RenderServices& rs)
{
    static const GUID kGuid = GUID::Derive(GUID::Null(), "engine/primitive/billboard_plane");
    auto& reg = rs.GetMeshGPURegistry();
    if (reg.FindHandle(Rendering::MeshGPUKey{kGuid, 0}).IsValid())
        return kGuid;
    Mesh plane = Engine::Renderer::PrimitiveGenerator::GeneratePlane();
    reg.RegisterSubmesh(Rendering::MeshGPUKey{kGuid, 0}, plane);
    return kGuid;
}

// Get or create an unlit material with the thumbnail as albedo.
static GUID GetOrCreateThumbnailMaterial(Engine::Renderer::RenderServices& rs,
                                          AssetManager* assets,
                                          const std::string& slug,
                                          const std::filesystem::path& thumbnailPath)
{
    GUID matGuid = GUID::Derive(GUID::Null(), "engine/material/polyhaven_thumb/" + slug);

    if (rs.Materials().Registry().Find(matGuid))
        return matGuid;

    // Register the thumbnail PNG as a texture asset.
    GUID texGuid;
    if (assets)
    {
        auto& reg = assets->GetRegistry();
        std::string pathStr = thumbnailPath.string();
        texGuid = reg.GetAssetGUID(pathStr);
        if (texGuid.IsNull())
        {
            reg.RegisterAsset(pathStr);
            texGuid = reg.GetAssetGUID(pathStr);
        }
    }

    // The shared factory shape is what MaterialVariantCook registers offline,
    // so a compiler-less runtime (web) serves this compose from its cooked
    // cache. Only the texture VALUE varies per slug — the slot set must not.
    MaterialDocument doc = MaterialDocument::CreateUnlitTextured(slug + "_placeholder");
    if (!texGuid.IsNull())
        doc.textures["albedoMap"] = texGuid.ToString();

    rs.RegisterAndPrewarmMaterial(matGuid, doc);
    return matGuid;
}

PolyhavenPlaceholderFactory::PlaceholderResult PolyhavenPlaceholderFactory::Create(
    ECS::World& world,
    Engine::Renderer::RenderServices& rs,
    AssetManager* assets,
    const std::string& slug,
    const std::string& assetType,
    const Mathematics::Vector3& worldPos,
    const Mathematics::Vector3& cameraPos,
    const std::filesystem::path& thumbnailPath,
    ECS::EntityHandle parent,
    bool addPlaceholderComponent)
{
    PlaceholderResult result{};

    ECS::EntityHandle e = world.CreateEntity();
    if (!e.IsValid())
        return result;

    // Name: "<slug> (downloading...)"
    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    std::string displayName = slug + " (downloading...)";
    std::strncpy(nm.value, displayName.c_str(), sizeof(nm.value) - 1);
    world.AddComponentImmediate(e, nm);

    // Compute aspect-ratio scale from the thumbnail image.
    float scaleX = 1.0f, scaleY = 1.0f;
    if (!thumbnailPath.empty())
    {
        uint32_t imgW = 0, imgH = 0;
        if (ReadPngDimensions(thumbnailPath, imgW, imgH))
        {
            float aspect = static_cast<float>(imgW) / static_cast<float>(imgH);
            if (aspect > 1.0f)
                scaleY = 1.0f / aspect;
            else
                scaleX = aspect;
        }
    }

    // Transform at drop position, oriented toward camera
    Components::Transform xf{};
    xf.SetIdentity();
    xf.matrix[12] = worldPos.x;
    xf.matrix[13] = worldPos.y;
    xf.matrix[14] = worldPos.z;
    OrientBillboard(xf, worldPos, cameraPos, scaleX, scaleY);
    world.AddComponentImmediate(e, xf);

    // Billboard plane mesh with thumbnail material (dedicated mesh GUID,
    // not PlaneGuid, to avoid sharing identity with the default scene plane).
    auto meshGuid = GetOrRegisterBillboardMesh(rs);
    GUID matGuid;
    if (!thumbnailPath.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(thumbnailPath, ec))
            matGuid = GetOrCreateThumbnailMaterial(rs, assets, slug, thumbnailPath);
    }
    if (matGuid.IsNull())
    {
        // No thumbnail — create a dim unlit placeholder material so it doesn't
        // appear as a bright light in the scene.
        matGuid = GetOrCreateThumbnailMaterial(rs, nullptr, slug, {});
    }

    auto mr = Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(&rs, meshGuid, matGuid);
    world.AddComponentImmediate(e, mr);
    world.AddComponentImmediate(e,
        Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(&rs, meshGuid));

    // PolyhavenPlaceholder tag component (skipped for temporary drag previews)
    if (addPlaceholderComponent)
    {
        Components::PolyhavenPlaceholder placeholder{};
        std::strncpy(placeholder.slug, slug.c_str(), sizeof(placeholder.slug) - 1);
        std::strncpy(placeholder.assetType, assetType.c_str(), sizeof(placeholder.assetType) - 1);
        world.AddComponentImmediate(e, placeholder);
    }

    // A new placeholder lands at the bottom of the hierarchy; the real model
    // that replaces it inherits this order (ReplacePlaceholder copies it).
    world.AddComponentImmediate(e, Editor::NextHierarchyOrderAtBottom(&world));

    // Parent if specified
    if (parent.IsValid())
    {
        Components::Parent p{};
        p.parent = parent;
        world.AddComponentImmediate(e, p);
    }

    result.entity = e;
    return result;
}

void PolyhavenPlaceholderFactory::OrientToFaceCamera(
    ECS::World& world, ECS::EntityHandle entity,
    const Mathematics::Vector3& worldPos,
    const Mathematics::Vector3& cameraPos)
{
    if (auto* xf = world.GetComponentForWrite<Components::Transform>(entity))
    {
        // Preserve the existing column scales (aspect ratio) when re-orienting.
        float sx = std::sqrt(xf->matrix[0] * xf->matrix[0] + xf->matrix[1] * xf->matrix[1] + xf->matrix[2] * xf->matrix[2]);
        float sy = std::sqrt(xf->matrix[8] * xf->matrix[8] + xf->matrix[9] * xf->matrix[9] + xf->matrix[10] * xf->matrix[10]);
        if (sx < 1e-6f) sx = 1.0f;
        if (sy < 1e-6f) sy = 1.0f;
        OrientBillboard(*xf, worldPos, cameraPos, sx, sy);
    }
}

} // namespace GameEngine
