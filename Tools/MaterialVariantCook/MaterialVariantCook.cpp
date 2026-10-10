// MaterialVariantCook — offline compilation of a project's material shader
// variants into a relocatable shader cache.
//
// A runtime without a shader compiler (wasm) can still bring materials up: it
// composes the same source and addresses the same content-derived cache key, so
// every variant this tool wrote is served straight from the cache. What it
// cannot do is compile a variant nobody cooked — so what is enumerated here is
// exactly what that runtime can draw.
//
//     MaterialVariantCook --project <dir> --engine-shaders <dir> [--cache <dir>]
//                         [--package-shaders <dir>]...
//                         [--scan <dir> [--scan-alias <alias>]]... [--verify]
//                         [--web --shadercook <shadercook.py>]
//                         [--particle-row <variant>]
//
// The cache root defaults to <project>/.Cache/Shaders — the location the
// runtime probes — so the cooked tree ships by copying the project.
//
// --scan adds another root's materials. A graph material's generated surface
// is named "<alias>/<path under its root>", which the runtime derives from its
// mount table, so a scan root's alias must be the one the runtime mounts it
// under. A root that a package manifest names as its assets folder takes that
// package's alias, derived as the package resolver derives it; --scan-alias
// names any other root. Aliases are spelled and checked as
// AssetManager::RegisterSource spells and checks them. The --project root is
// "project".
//
// --verify re-resolves every cooked variant with the compiler switched off,
// which is the same code path the shipped runtime takes. A variant that only
// resolves with a compiler present is a cook that did not actually land.
//
// --web cooks the WebGPU-class profile: the compat shader profile (no
// buffer_device_address, no descriptor indexing — the two things naga refuses
// in the desktop material SPIR-V) plus a "<stage>-wgsl" chunk per variant, so
// a browser that ingests no SPIR-V still gets a pipeline. The compat profile is
// a compile input, so a web cache and a desktop cache address different keys
// and can share a cache root without colliding.
//
// --particle-row cooks only the named row of the particle request set
// (Particles::ParticleRenderVariants) for each particle shape instead of the
// whole set: a check that every shape composes and translates, at a fraction
// of the cost. A cache cooked with it is incomplete for particles; an export
// cooks the whole set.

#include "AssetCore/PathNormalization.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetSourceAlias.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Packages/PackageManifest.h"
#include "Assets/Packages/PackageResolver.h"
#include "Engine/Rendering/MaterialShaderPackageBuilder.h"
#include "Particles/Rendering/ParticleMaterials.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/Materials/MaterialShaderIncludeRoots.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "CBTTerrainECS/CBTTerrainMaterial.h"
#include "EZTreeECS/EZTreeRuntimeMaterials.h"
#include "TerrainGrass/GrassDrawMode.h"
#include "Ocean/OceanSurfaceMaterial.h"
#include "WebWgslCook.h"
#include "MeshVariantTable.h"
#include "VariantRequests.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

// One shader variant of one material: the pass keywords the renderer will ask
// for, and the vertex layout the mesh feeding it carries.
using GameEngine::Tools::MaterialCook::VariantRequest;
using GameEngine::Tools::MaterialCook::ImportedMeshMaterialTarget;
using GameEngine::Tools::MaterialCook::kDepthMaskRows;
using GameEngine::Tools::MaterialCook::kImportedMeshMaterialTargets;
using GameEngine::Tools::MaterialCook::kMeshVariants;

// A --scan root, the --scan-alias given for it, and the alias its generated
// surfaces are named under once ResolveScanAlias has settled it.
struct ScanRoot
{
    fs::path Path;
    std::optional<std::string> GivenAlias;
    std::string Alias;
};

struct Options
{
    fs::path Project;
    fs::path EngineShaders;
    fs::path Cache;
    std::vector<fs::path> PackageShaders;
    std::vector<ScanRoot> ScanRoots;
    fs::path ShaderCookScript;
    bool Verify = false;
    bool Web = false;
    // The one particle request-set row each particle shape cooks under --particle-row.
    std::optional<VariantRequest> ParticleRow;
};

// One material the cook is responsible for. A project's .material assets are
// the obvious members; the engine's built-in default PBR document is the
// non-obvious one — every primitive and every model without an assigned
// material draws with it, so a cache without its variants renders nothing even
// though the project looks fully cooked. A target may carry its own variant
// list (built-in contributor materials request one specific keyword set at
// registration, not the mesh-material table); empty means kMeshVariants. Rows are
// expanded with the optional pass keywords (contact shadows) unless the target
// lists exactly what its pass draws with (`ExpandsOptionalPasses` false).
// `ImportedMeshMaterial` marks the documents an imported model's materials
// compose as (the built-in default PBR family): the only ones a mesh with a
// COLOR_0 stream reaches on a cooked-only runtime, so the only ones that cook
// the vertex-colour rows.
struct CookSource
{
    std::string Name;
    fs::path MaterialPath;
    MaterialDocument Document;
    std::vector<VariantRequest> Variants;
    bool ImportedMeshMaterial = false;
    bool ExpandsOptionalPasses = true;
};

// Preparation precedes variant enumeration because graph tags shape the document.
struct CookTarget
{
    std::string Name;
    fs::path MaterialPath;
    Engine::Renderer::MaterialShaderPackageBuilder Builder;
    std::vector<VariantRequest> Variants;
    bool ImportedMeshMaterial = false;
    bool ExpandsOptionalPasses = true;
};

std::vector<VariantRequest> VariantsFor(const CookTarget& target, bool web)
{
    const MaterialDocument& document = target.Builder.GetDocument();
    auto rows = GameEngine::Tools::MaterialCook::SelectVariantRequests(
        kMeshVariants, target.Variants, kDepthMaskRows, document.alphaMode);
    GameEngine::Tools::MaterialCook::ExpandVertexColorVariants(
        rows, target.ImportedMeshMaterial && !document.ignoreVertexColor);
    if (target.ExpandsOptionalPasses)
        GameEngine::Tools::MaterialCook::ExpandOptionalPassVariants(rows);
    const auto heightMap = document.textures.find(kParallaxHeightMapSlot);
    const bool bindsHeightMap =
        target.Variants.empty() && heightMap != document.textures.end() && !heightMap->second.empty();
    GameEngine::Tools::MaterialCook::ExpandParallaxDepthVariants(rows, bindsHeightMap, web);
    return rows;
}

// The alias the runtime mounts a package under when its manifest names `root`
// as the package's assets folder: SanitizePackageAlias of the manifest's name,
// as PackageResolver derives it. Empty when no manifest above `root` names it.
// A package.json that is not an engine package manifest names no root.
std::string ManifestAlias(const fs::path& root)
{
    const fs::path mountRoot = AssetPaths::NormalizeMountRoot(root);
    for (fs::path directory = mountRoot;; directory = directory.parent_path())
    {
        std::error_code ec;
        const fs::path manifestFile = directory / "package.json";
        PackageManifest manifest;
        std::string notAPackageManifest;
        if (fs::is_regular_file(manifestFile, ec) &&
            TryLoadPackageManifest(manifestFile, manifest, notAPackageManifest) &&
            !manifest.AssetsDir.empty() &&
            AssetPaths::NormalizeMountRoot(directory / manifest.AssetsDir) == mountRoot)
            return SanitizePackageAlias(manifest.Name);
        if (directory == directory.parent_path())
            return {};
    }
}

// Settles a scan root's alias, the given one or its package's, in the spelling
// the runtime registers, and refuses one the runtime could not mount.
bool ResolveScanAlias(ScanRoot& scan, std::string& error)
{
    const std::string alias = scan.GivenAlias ? *scan.GivenAlias : ManifestAlias(scan.Path);
    if (!scan.GivenAlias && alias.empty())
    {
        error = "--scan " + scan.Path.string() +
                ": no package manifest names this folder as its package's assets folder, so the "
                "alias the runtime mounts it under is unknown; pass --scan-alias <alias>";
        return false;
    }
    scan.Alias = AssetPathSyntax::NormalizeAssetSourceAlias(alias);
    if (!AssetPathSyntax::IsValidAssetSourceAlias(scan.Alias))
    {
        error = "--scan " + scan.Path.string() + ": the alias '" + alias +
                "' is not a valid source alias; an alias is lower-cased and must be non-empty "
                "and use only letters, digits, '-' and '_'";
        return false;
    }
    return true;
}

bool ParseOptions(int argc, char** argv, Options& out, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&](auto& dst) {
            if (i + 1 >= argc)
            {
                error = arg + " needs a value";
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--project")
        {
            if (!value(out.Project)) return false;
        }
        else if (arg == "--engine-shaders")
        {
            if (!value(out.EngineShaders)) return false;
        }
        else if (arg == "--cache")
        {
            if (!value(out.Cache)) return false;
        }
        else if (arg == "--package-shaders")
        {
            fs::path p;
            if (!value(p)) return false;
            out.PackageShaders.push_back(std::move(p));
        }
        else if (arg == "--scan")
        {
            ScanRoot scan;
            if (!value(scan.Path)) return false;
            out.ScanRoots.push_back(std::move(scan));
        }
        else if (arg == "--scan-alias")
        {
            if (out.ScanRoots.empty() || out.ScanRoots.back().GivenAlias)
            {
                error = "--scan-alias names the --scan root just before it";
                return false;
            }
            std::string alias;
            if (!value(alias)) return false;
            out.ScanRoots.back().GivenAlias = std::move(alias);
        }
        else if (arg == "--shadercook")
        {
            if (!value(out.ShaderCookScript)) return false;
        }
        else if (arg == "--verify")
        {
            out.Verify = true;
        }
        else if (arg == "--web")
        {
            out.Web = true;
        }
        else if (arg == "--particle-row")
        {
            std::string name;
            if (!value(name)) return false;
            const auto rows = Particles::ParticleRenderVariants();
            const auto row = std::find_if(rows.begin(), rows.end(),
                                          [&](const Particles::ParticleRenderVariant& r) { return r.Name == name; });
            if (row == rows.end())
            {
                error = "--particle-row " + name +
                        ": not a row of the particle request set; name one such as 'base' or "
                        "'forward-color'";
                return false;
            }
            out.ParticleRow = VariantRequest{row->Name, row->PassKeywords, row->VertexFlags};
        }
        else
        {
            error = "unknown argument: " + arg;
            return false;
        }
    }

    if (out.Project.empty() || out.EngineShaders.empty())
    {
        error = "--project and --engine-shaders are required";
        return false;
    }
    if (!fs::is_directory(out.Project))
    {
        error = "not a directory: " + out.Project.string();
        return false;
    }
    std::unordered_set<std::string> aliases{std::string(kAssetSourceAliasProject)};
    for (ScanRoot& scan : out.ScanRoots)
    {
        if (!fs::is_directory(scan.Path))
        {
            error = "not a directory: " + scan.Path.string();
            return false;
        }
        if (!ResolveScanAlias(scan, error))
            return false;
        if (!aliases.insert(scan.Alias).second)
        {
            error = "--scan " + scan.Path.string() + ": the alias '" + scan.Alias +
                    "' is already taken (the --project root is 'project'); name this root "
                    "with --scan-alias <alias>, the alias the runtime mounts it under";
            return false;
        }
    }
    if (!fs::is_directory(out.EngineShaders))
    {
        error = "not a directory: " + out.EngineShaders.string();
        return false;
    }
    if (out.Web && out.ShaderCookScript.empty())
    {
        error = "--web needs --shadercook <path to Tools/ShaderCook/shadercook.py>";
        return false;
    }
    if (!out.ShaderCookScript.empty() && !fs::is_regular_file(out.ShaderCookScript))
    {
        error = "not a file: " + out.ShaderCookScript.string();
        return false;
    }
    if (out.Cache.empty())
        out.Cache = out.Project / ".Cache" / "Shaders";
    return true;
}

std::vector<fs::path> FindMaterials(const fs::path& project)
{
    std::vector<fs::path> materials;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(project, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
            break;
        // The cache is output, never input.
        if (it->is_directory(ec) && it->path().filename() == ".Cache")
        {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().extension() == ".material")
            materials.push_back(it->path());
    }
    std::sort(materials.begin(), materials.end());
    return materials;
}

// Every material the project can draw with: its own assets, plus the engine's
// built-in default PBR document, whose stand-in path is anchored under the
// cache root exactly as the runtime anchors it — a directory holding no
// shaders, so it can never shadow the project/package/engine resolution chain.
// Preparation materializes graph surfaces and extends the shared include roots.
// Targets whose preparation fails are reported and excluded from compilation.
std::vector<CookTarget> CollectTargets(const fs::path& project, MaterialBuildContext& context,
                                       bool web, const std::vector<ScanRoot>& scanRoots,
                                       const std::optional<VariantRequest>& particleRow,
                                       std::vector<std::string>& outErrors)
{
    const fs::path& cacheRoot = context.CacheRoot;
    std::vector<CookSource> targets;
    std::vector<fs::path> materialPaths = FindMaterials(project);
    for (const ScanRoot& scan : scanRoots)
    {
        std::vector<fs::path> extraMaterials = FindMaterials(scan.Path);
        materialPaths.insert(materialPaths.end(), extraMaterials.begin(), extraMaterials.end());
    }
    std::sort(materialPaths.begin(), materialPaths.end());
    materialPaths.erase(std::unique(materialPaths.begin(), materialPaths.end()),
                        materialPaths.end());
    for (const fs::path& materialPath : materialPaths)
    {
        MaterialAsset asset(GUID::Generate(), materialPath);
        if (!asset.Load())
        {
            std::string message = materialPath.filename().string() + ": unreadable";
            for (const std::string& e : asset.GetErrors())
                message += "\n        " + e;
            outErrors.push_back(std::move(message));
            continue;
        }
        targets.push_back({materialPath.stem().string(), materialPath, asset.GetDocument(), {}});
    }
    // The standard-PBR stand-ins for imported mesh materials, one per alpha mode
    // (kImportedMeshMaterialTargets); VariantsFor appends the depth-mask rows to
    // the Mask one.
    for (const ImportedMeshMaterialTarget& imported : kImportedMeshMaterialTargets)
    {
        MaterialDocument document = MaterialDocument::CreateDefaultPBR(imported.DocumentName);
        document.alphaMode = imported.AlphaMode;
        targets.push_back({imported.Name, cacheRoot / "Synthetic" / imported.File, std::move(document),
                           std::vector<VariantRequest>(imported.Rows.begin(), imported.Rows.end()), true});
    }

    // Unlit textured placeholder (Polyhaven thumbnail billboards and any other
    // CreateUnlitTextured consumer): registered at runtime per asset, but every
    // instance composes the same source, so one target serves them all.
    targets.push_back({"unlit-textured-placeholder",
                       cacheRoot / "Synthetic" / "unlit_textured.material",
                       MaterialDocument::CreateUnlitTextured("UnlitTextured"), {}});

    // Particle emitters register their materials at runtime from synthetic
    // documents too, and a cooked-only runtime cannot compile the miss. The
    // shapes come from the builder extraction uses, so the composed source (and
    // therefore the variant key) cannot drift; an emitter's values, textures and
    // source material change no program, so one target per shape serves them all.
    // Each shape cooks the particle renderer's own request set, exactly: the
    // late transparent pass draws no optional pass keyword.
    {
        const std::vector<MaterialDocument> shapes = Particles::ParticleRenderMaterialShapes();
        std::vector<VariantRequest> rows;
        if (particleRow)
            rows.push_back(*particleRow);
        else
            for (const Particles::ParticleRenderVariant& variant : Particles::ParticleRenderVariants())
                rows.push_back({variant.Name, variant.PassKeywords, variant.VertexFlags});
        for (size_t index = 0; index < shapes.size(); ++index)
        {
            const std::string name = "particles-" + std::to_string(index);
            targets.push_back({name, cacheRoot / "Synthetic" / (name + ".material"), shapes[index], rows, false, false});
        }
    }

    // The ocean surface registers at runtime from a synthetic document (no
    // .material asset) with exactly one keyword set, so it cooks its own row:
    // the registration pipeline is the only variant the renderer requests.
    // The document and keywords come from the Ocean module so the cook and the
    // runtime cannot drift.
    {
        CookSource ocean{"ocean-surface", cacheRoot / "Synthetic" / "ocean_surface.material",
                         Ocean::BuildOceanSurfaceMaterialDocument(), {}};
        ocean.Variants.push_back({"ocean-forward", Ocean::OceanSurfaceMaterialKeywords(web),
                                  VertexAttributeFlags::None});
        targets.push_back(std::move(ocean));
    }

    // CBT terrain, same shape as the ocean: a synthetic document registered at runtime,
    // taken from the module so the cook and the feature cannot drift. It cannot use the
    // default variant table — customVertexShader forces vertexFlags = None, so every row
    // is one variant rather than four vertex buckets, and the material is not Instanced.
    // The pass re-keys apply here exactly as they do to world geometry: an AO or SSR
    // volume flips the whole colour pass to a keyword set, and a terrain with no cooked
    // row for it disappears rather than degrades.
    {
        using CBTTerrainECS::BuildCBTTerrainMaterialDocument;
        using CBTTerrainECS::CBTTerrainMaterialKeywords;
        const MaterialKeyword base = CBTTerrainMaterialKeywords();
        CookSource terrain{"cbt-terrain", cacheRoot / "Synthetic" / "cbt_terrain.material",
                           BuildCBTTerrainMaterialDocument(), {}};
        // The keyword-less variant registration prewarms before any pass has asked.
        terrain.Variants.push_back({"cbt-base", MaterialKeyword::None,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward", base, VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-sssr",
                                    base | MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-gtao", base | MaterialKeyword::GTAO,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-gtao-sssr",
                                    base | MaterialKeyword::GTAO |
                                        MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        // A DDGIVolume resolved for the view re-keys the colour pass exactly like AO
        // or SSR do (this block's own comment above), so DDGI crosses every row above
        // it the same way GTAO and SSSR already cross each other: on its own, and
        // combined with each of the other two. Missed here, a terrain under a DDGI
        // volume has no cooked row and disappears on a cooked-only runtime — the
        // black-screen report this table exists to prevent, just for a keyword this
        // table had not been extended to yet.
        terrain.Variants.push_back({"cbt-forward-ddgi", base | MaterialKeyword::DDGI,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-ddgi-sssr",
                                    base | MaterialKeyword::DDGI |
                                        MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-ddgi-gtao",
                                    base | MaterialKeyword::DDGI | MaterialKeyword::GTAO,
                                    VertexAttributeFlags::None});
        terrain.Variants.push_back({"cbt-forward-ddgi-gtao-sssr",
                                    base | MaterialKeyword::DDGI | MaterialKeyword::GTAO |
                                        MaterialKeyword::SSSRNormalRoughness,
                                    VertexAttributeFlags::None});
        targets.push_back(std::move(terrain));
    }

    // Terrain grass, same shape as the terrain above: a synthetic document taken from the module
    // so cook and feature cannot drift. vertexFlags = None, matching what the feature actually
    // submits — the blade card is a bare two-float buffer the vertex modifier expands, not a
    // standard mesh layout, so none of the StandardMesh buckets describe it.
    //
    // Web only, and deliberately so. Grass is cooked because a browser ships no compiler; every
    // other target compiles this material at runtime and always has. Cooking it for the default
    // profile as well would mean giving that profile's adapter a uniform block it never declared,
    // which is a change to shaders that are working — so the cook does not ask for it there.
    if (web)
    {
        using TerrainGrass::GrassDrawMode;
        // The per-mode document TerrainGrassRenderFeature registers, mirrored here because that
        // builder is private to the feature TU. Only the fields the cook keys on are reproduced.
        const auto grassDocFor = [](GrassDrawMode mode) {
            MaterialDocument doc{};
            doc.lightingModel = "StandardPBR";
            doc.surfaceShader = "TerrainGrass/terrain_grass_surface.glsl";
            doc.vertexModifier = "TerrainGrass/terrain_grass_vertex_modifier.glsl";
            doc.doubleSided = true;
            doc.properties["roughness"] = 0.9f;
            switch (mode)
            {
            case GrassDrawMode::Blend:
                doc.materialName = "Terrain/Grass";
                doc.alphaMode = MaterialAlphaMode::Blend;
                break;
            case GrassDrawMode::Opaque:
                doc.materialName = "Terrain/GrassOpaque";
                doc.alphaMode = MaterialAlphaMode::Opaque;
                doc.keywords = {"GRASS_OPAQUE"};
                break;
            case GrassDrawMode::Dither:
                doc.materialName = "Terrain/GrassDither";
                doc.alphaMode = MaterialAlphaMode::Opaque;
                doc.keywords = {"GRASS_DITHER"};
                break;
            case GrassDrawMode::DitherA2C:
                doc.materialName = "Terrain/GrassA2C";
                doc.alphaMode = MaterialAlphaMode::Opaque;
                doc.keywords = {"GRASS_A2C"};
                break;
            }
            return doc;
        };
        // The feature submits the blade draw with HasVertexMod already set, and
        // MaterialBuildService adds HasVertexOutputMod on top (the modifier
        // declares the output capability). The cook must request the same pair or
        // its rows key on a different define set — HAS_VERTEX_MODIFIER missing —
        // and the cooked-only runtime resolves nothing for the draw.
        const MaterialKeyword base =
            MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::Instanced |
            MaterialKeyword::IBL | MaterialKeyword::ProceduralVertexOutput |
            MaterialKeyword::HasVertexMod;
        // None resolves to StandardMesh inside the build (MaterialBuildService),
        // which is what the blade draw asks for.
        const VertexAttributeFlags bladeFlags = VertexAttributeFlags::None;
        // All three draw-mode materials: the feature picks one per view at runtime
        // (authored Blend vs Dither, and Dither splits on the view's sample count),
        // so a cooked-only runtime needs every mode it could resolve to.
        struct GrassModeTarget
        {
            GrassDrawMode Mode;
            const char* TargetName;
            const char* FileName;
        };
        constexpr GrassModeTarget kGrassModes[] = {
            {GrassDrawMode::Blend, "terrain-grass", "terrain_grass.material"},
            {GrassDrawMode::Opaque, "terrain-grass-opaque", "terrain_grass_opaque.material"},
            {GrassDrawMode::Dither, "terrain-grass-dither",
             "terrain_grass_dither.material"},
            {GrassDrawMode::DitherA2C, "terrain-grass-a2c",
             "terrain_grass_a2c.material"},
        };
        for (const GrassModeTarget& gm : kGrassModes)
        {
            CookSource grass{gm.TargetName, cacheRoot / "Synthetic" / gm.FileName,
                             grassDocFor(gm.Mode), {}};
            grass.Variants.push_back({"grass-base", MaterialKeyword::None, bladeFlags});
            grass.Variants.push_back({"grass-forward", base, bladeFlags});
            grass.Variants.push_back({"grass-forward-sssr",
                                      base | MaterialKeyword::SSSRNormalRoughness, bladeFlags});
            grass.Variants.push_back(
                {"grass-forward-gtao", base | MaterialKeyword::GTAO, bladeFlags});
            grass.Variants.push_back({"grass-forward-gtao-sssr",
                                      base | MaterialKeyword::GTAO |
                                          MaterialKeyword::SSSRNormalRoughness,
                                      bladeFlags});
            // WorldRenderNode applies DDGI to every material in the view,
            // including each grass draw mode. Cook its combinations with AO
            // and reflections so a compiler-less runtime can resolve them.
            grass.Variants.push_back(
                {"grass-forward-ddgi", base | MaterialKeyword::DDGI, bladeFlags});
            grass.Variants.push_back({"grass-forward-ddgi-sssr",
                                      base | MaterialKeyword::DDGI |
                                          MaterialKeyword::SSSRNormalRoughness,
                                      bladeFlags});
            grass.Variants.push_back({"grass-forward-ddgi-gtao",
                                      base | MaterialKeyword::DDGI | MaterialKeyword::GTAO,
                                      bladeFlags});
            grass.Variants.push_back({"grass-forward-ddgi-gtao-sssr",
                                      base | MaterialKeyword::DDGI | MaterialKeyword::GTAO |
                                          MaterialKeyword::SSSRNormalRoughness,
                                      bladeFlags});
            targets.push_back(std::move(grass));
        }

        // Tree Generator runtime materials: extraction always attaches
        // ez_tree_wind (even when wind is off — the shader early-outs) and
        // leaves always use Mask + ez_tree_leaves.glsl. Scanning the package
        // .material files cooks a different intern identity (no modifier,
        // standard_pbr leaves), so the canopy, bark textures, and wind all
        // miss on a compiler-less runtime. Same builders the extractor uses.
        {
            using EZTreeECS::MakeBarkRuntimeMaterialShape;
            using EZTreeECS::MakeLeafRuntimeMaterialShape;
            using EZTreeECS::MakeTrellisRuntimeMaterialShape;
            targets.push_back({"eztree-bark", cacheRoot / "Synthetic" / "eztree_bark.material",
                               MakeBarkRuntimeMaterialShape(), {}});
            targets.push_back({"eztree-leaves", cacheRoot / "Synthetic" / "eztree_leaves.material",
                               MakeLeafRuntimeMaterialShape(), {}});
            targets.push_back({"eztree-trellis", cacheRoot / "Synthetic" / "eztree_trellis.material",
                               MakeTrellisRuntimeMaterialShape(), {}});
        }
    }
    std::vector<CookTarget> preparedTargets;
    preparedTargets.reserve(targets.size());
    for (CookSource& source : targets)
    {
        Engine::Renderer::MaterialShaderPackageBuilder builder(
            std::move(source.Document), source.MaterialPath, source.Name, context);
        if (!builder.IsPrepared())
        {
            for (const std::string& error : builder.GetErrors())
                outErrors.push_back(source.Name + ": " + error);
            continue;
        }
        preparedTargets.push_back({std::move(source.Name), std::move(source.MaterialPath),
                                   std::move(builder), std::move(source.Variants), source.ImportedMeshMaterial,
                                   source.ExpandsOptionalPasses});
    }
    return preparedTargets;
}

// The logger's drain thread shares the console and writes a message and its
// newline separately, so a result line written in pieces can be split by a log
// line. Each result line, with its indented detail lines, is composed first
// and written with one insertion.
std::string ResultLines(std::string head, const std::vector<std::string>& details)
{
    head += '\n';
    for (const std::string& detail : details)
        head += "        " + detail + '\n';
    return head;
}

void PrintUsage(std::ostream& out)
{
    out << "usage: MaterialVariantCook --project <dir> --engine-shaders <dir>\n"
           "                           [--cache <dir>] [--package-shaders <dir>]...\n"
           "                           [--scan <dir> [--scan-alias <alias>]]...\n"
           "                           [--verify] [--web --shadercook <path>]\n"
           "                           [--particle-row <variant>]\n"
           "--scan-alias names the --scan root before it in generated graph surface\n"
           "names and must be the alias the runtime mounts that root under. Without\n"
           "it, a root that a package manifest names as its assets folder takes that\n"
           "package's alias; any other root needs --scan-alias. An alias is\n"
           "lower-cased and uses letters, digits, '-' and '_'. The --project root\n"
           "is 'project'.\n";
}

} // namespace

int main(int argc, char** argv)
{
    Logger::Log::Initialize(Logger::Log::Config{});

    Options options{};
    std::string error;
    if (argc == 2 && std::string_view(argv[1]) == "--help")
    {
        PrintUsage(std::cout);
        return 0;
    }
    if (!ParseOptions(argc, argv, options, error))
    {
        std::cerr << "MaterialVariantCook: " << error << "\n";
        PrintUsage(std::cerr);
        return 2;
    }
    if (!ShaderCompileService::IsCompilerAvailable())
    {
        std::cerr << "MaterialVariantCook: this build has no shader compiler; nothing can be "
                     "cooked. Build the tool from a desktop configuration with shaderc.\n";
        return 2;
    }

    MaterialBuildContext context{};
    context.AdapterShaderDir = fs::absolute(options.EngineShaders);
    context.ProjectRoots = {fs::absolute(options.Project)};
    context.AssetSourceRoots.push_back(
        {std::string(kAssetSourceAliasProject), AssetPaths::NormalizeMountRoot(options.Project)});
    for (const ScanRoot& scan : options.ScanRoots)
        context.AssetSourceRoots.push_back({scan.Alias, AssetPaths::NormalizeMountRoot(scan.Path)});
    context.PackageShaderDirs = options.PackageShaders;
    // A project's own Shaders/ dir is a package shader root like any other.
    const fs::path projectShaders = fs::absolute(options.Project) / "Shaders";
    if (fs::is_directory(projectShaders))
        context.PackageShaderDirs.push_back(projectShaders);
    context.CacheRoot = fs::absolute(options.Cache);

    // The compile inputs of the device class the cook serves, set before the
    // first build the way the runtime sets them from the profile its device
    // resolves: a desktop device resolves the full profile, every feature on
    // (the interpolation functions included); a browser resolves the compat
    // profile, which keeps the cooked SPIR-V free of the buffer_device_address
    // and descriptor-indexing constructs naga refuses and of interpolation
    // functions WGSL does not have. An input set differently here from the
    // runtime changes the cache key, and that runtime then misses every
    // program cooked for it.
    Rendering::RendererProfile servedProfile{};
    if (options.Web)
    {
        servedProfile.ActiveTier = Rendering::RendererProfile::Tier::Compatibility;
        servedProfile.UseInterpolationFunctions = false;
    }
    Rendering::ApplyShaderCompileProfile(servedProfile);

    size_t failed = 0;
    std::vector<std::string> loadErrors;
    const std::vector<CookTarget> targets =
        CollectTargets(options.Project, context, options.Web, options.ScanRoots,
                       options.ParticleRow, loadErrors);
    for (const std::string& e : loadErrors)
    {
        std::cerr << ResultLines("  FAIL " + e, {});
        ++failed;
    }

    // Outside the cache root: the cache ships with the content, and the WGSL
    // chain's intermediates are debugging material, not shipped artifacts. A
    // failure names the scratch file it wrote so it stays inspectable.
    const fs::path wgslScratch = fs::temp_directory_path() / "MaterialVariantCookWeb";

    size_t cooked = 0;
    size_t webTranslationsReused = 0;
    std::unordered_set<std::string> webCookedPackages;
    for (const CookTarget& target : targets)
    {
        for (const VariantRequest& variant : VariantsFor(target, options.Web))
        {
            const MaterialBuildResult result = target.Builder.Build(
                ShaderSourceKind::SpirV, variant.PassKeywords, variant.VertexFlags);
            if (!result.success)
            {
                std::cerr << ResultLines("  FAIL " + target.Name + " [" + variant.Name + "]",
                                         result.errors);
                ++failed;
                continue;
            }

            bool reuseWebTranslation = false;
            if (options.Web && webCookedPackages.contains(result.generatedShaderPkgPath))
            {
                // BuildMaterialToShaderPackage has validated this content key
                // and its include dependencies. Shared materials can reuse the
                // WGSL translated in this run. A stale/rebuilt SPIR-V package
                // has no WGSL chunks and must go through translation again.
                ShaderPackage pkg{};
                reuseWebTranslation = LoadShaderPkg(result.generatedShaderPkgPath,
                                                     ShaderSourceKind::SpirV, pkg)
                    && pkg.wgslStages.contains("vs") && pkg.wgslStages.contains("fs");
            }
            if (options.Web && !reuseWebTranslation)
            {
                Tools::WebWgslCookRequest wgsl{};
                wgsl.ShaderCookScript = fs::absolute(options.ShaderCookScript);
                wgsl.ScratchDir = wgslScratch;
                wgsl.PackagePath = result.generatedShaderPkgPath;
                wgsl.VertexSource = result.composedVertexSource;
                wgsl.FragmentSource = result.composedFragmentSource;
                wgsl.Defines = result.composedDefines;
                wgsl.IncludeRoots =
                    BuildMaterialIncludeRoots(context, target.MaterialPath.parent_path());
                wgsl.DebugName = target.Name + "_" + variant.Name;
                std::string wgslError;
                if (!Tools::CookWebWgslIntoPackage(wgsl, wgslError))
                {
                    std::cerr << ResultLines(
                        "  FAIL " + target.Name + " [" + variant.Name + "] WGSL", {wgslError});
                    ++failed;
                    continue;
                }
                // Do not reuse entries from a prior run: this run's compiler
                // and translation script must validate every distinct package.
                webCookedPackages.insert(result.generatedShaderPkgPath);
            }
            if (reuseWebTranslation)
                ++webTranslationsReused;

            ++cooked;
            std::cout << ResultLines("  ok   " + target.Name + " [" + variant.Name + "] -> " +
                                         result.generatedShaderPkgPath,
                                     {});
        }
    }

    if (options.Verify && failed == 0)
    {
        // Exactly what the shipped runtime does: resolve every variant with no
        // compiler in reach. A miss here means the cook wrote an entry the
        // runtime does not address — the failure mode this whole path exists to
        // prevent, and one a compiler-present run would have papered over.
        ShaderCompileService::SetCompilerAvailable(false);
        size_t unserved = 0;
        for (const CookTarget& target : targets)
        {
            for (const VariantRequest& variant : VariantsFor(target, options.Web))
            {
                const MaterialBuildResult result = target.Builder.Build(
                    ShaderSourceKind::SpirV, variant.PassKeywords, variant.VertexFlags);
                // The WGSL lives in the package on disk, not in the build
                // result: the compile service's SPIR-V-shaped result carries no
                // WGSL, and a browser build substitutes the chunks at parse
                // time. Checking the shipped artifact is the honest check.
                bool servable = result.success;
                if (servable && options.Web)
                {
                    ShaderPackage pkg{};
                    std::string loadError;
                    servable = LoadShaderPkg(result.generatedShaderPkgPath,
                                             ShaderSourceKind::SpirV, pkg, &loadError)
                               && pkg.wgslStages.count("vs") != 0
                               && pkg.wgslStages.count("fs") != 0;
                }
                if (servable)
                    continue;
                std::cerr << ResultLines("  UNSERVED " + target.Name + " [" + variant.Name + "]" +
                                             (result.success ? " (no WGSL chunks in the package)"
                                                             : ""),
                                         result.errors);
                ++unserved;
            }
        }
        ShaderCompileService::SetCompilerAvailable(true);
        if (unserved != 0)
        {
            std::cerr << "MaterialVariantCook: " << unserved
                      << " cooked variant(s) are NOT servable without a compiler\n";
            return 1;
        }
        std::cout << "verify: every cooked variant resolves with no compiler\n";
    }

    std::cout << "MaterialVariantCook: " << cooked << " variant(s) cooked from " << targets.size()
              << " material(s) into " << context.CacheRoot << "\n";
    if (options.Web)
        std::cout << "MaterialVariantCook: reused " << webTranslationsReused
                  << " shared WGSL translation(s) within this run\n";
    if (failed != 0)
    {
        std::cerr << "MaterialVariantCook: " << failed << " variant(s) failed\n";
        return 1;
    }
    return 0;
}
