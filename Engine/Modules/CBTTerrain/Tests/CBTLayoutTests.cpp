// Static layout contract: the CBTLayout.h constants/struct sizes that must stay
// lockstep with cbt_layout.glsl. Most are enforced by static_assert at compile
// time; these runtime checks make the contract visible as test cases and guard
// the u32 sum-tree derivation for the default 128k pool.

#include <gtest/gtest.h>

#include "CBTTerrain/CBTDeepDecode.h"    // kDeepDecodeSubdiv (S2a GLSL mirror lock)
#include "CBTTerrain/CBTDemandTuning.h"  // kOffFrustumKeepOcc (pressure recover mark)
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTPoolHealth.h"    // kCBTSaturatedOccupancy (pressure full mark)
#include "CBTTerrain/CBTSphereRoots.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "Terrain/TerrainTypes.h" // TerrainGPUParams — the C++ twin of TerrainParamsEntry

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine::CBTTerrain;
// TerrainMaterialRecord + the terrain material defaults live in the Terrain module — the one both
// the CBT surface and the grass shaders read the table from.
using namespace GameEngine::Terrain;

// --- Minimal GLSL struct parser + std430 layout calculator -------------------------------------
// Presence checks ("the field is still spelled somewhere in the file") cannot see a reorder, an
// insert, or a scalar -> vecN change — each of which silently corrupts every GPU read of the
// mirror struct. These helpers parse the GLSL declarations in order and compute their std430
// offsets so the lock can compare them against offsetof() on the C++ twin. Only what the mirror
// structs actually use is supported (scalars, vecN, arrays, nested structs); anything else fails
// the test loudly rather than passing silently.
namespace
{

struct GlslField
{
    std::string Type;
    std::string Name;
    bool IsArray = false;
    uint32_t ArrayLen = 1;
};

struct Std430Layout
{
    uint32_t Size = 0;
    uint32_t Align = 0;
    std::vector<uint32_t> Offsets; // parallel to the field list it was computed from
};

std::string SlurpFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open())
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Drop `// ...` line comments so struct bodies parse as plain declarations.
std::string StripLineComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i <= src.size())
    {
        const size_t nl = src.find('\n', i);
        const size_t end = (nl == std::string::npos) ? src.size() : nl;
        const size_t slashes = src.find("//", i);
        const size_t cut = (slashes != std::string::npos && slashes < end) ? slashes : end;
        out.append(src, i, cut - i);
        out.push_back('\n');
        if (nl == std::string::npos)
            break;
        i = nl + 1;
    }
    return out;
}

// Body of `struct <name> { ... }`, braces balanced.
bool ExtractStructBody(const std::string& src, const std::string& structName, std::string& outBody)
{
    std::smatch m;
    if (!std::regex_search(src, m, std::regex("struct\\s+" + structName + "\\s*\\{")))
        return false;
    size_t i = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(0)); // past '{'
    const size_t start = i;
    int depth = 1;
    for (; i < src.size() && depth > 0; ++i)
    {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}')
            --depth;
    }
    if (depth != 0)
        return false;
    outBody = src.substr(start, i - 1 - start);
    return true;
}

// Field declarations in DECLARATION ORDER. `constants` resolves symbolic array lengths.
bool ParseGlslStructFields(const std::string& strippedSrc, const std::string& structName,
                           const std::map<std::string, uint32_t>& constants,
                           std::vector<GlslField>& out, std::string& outError)
{
    out.clear();
    std::string body;
    if (!ExtractStructBody(strippedSrc, structName, body))
    {
        outError = "struct " + structName + " not found";
        return false;
    }
    const std::regex declRe(
        R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*([A-Za-z0-9_]+)\s*\])?\s*$)");
    size_t start = 0;
    while (true)
    {
        const size_t semi = body.find(';', start);
        if (semi == std::string::npos)
            break;
        const std::string decl = body.substr(start, semi - start);
        start = semi + 1;
        if (decl.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        std::smatch m;
        if (!std::regex_match(decl, m, declRe))
        {
            outError = structName + ": unparsed declaration '" + decl + "'";
            return false;
        }
        GlslField f;
        f.Type = m[1].str();
        f.Name = m[2].str();
        if (m[3].matched)
        {
            f.IsArray = true;
            const std::string len = m[3].str();
            if (!len.empty() && std::isdigit(static_cast<unsigned char>(len[0])) != 0)
            {
                f.ArrayLen = static_cast<uint32_t>(std::stoul(len));
            }
            else
            {
                const auto it = constants.find(len);
                if (it == constants.end())
                {
                    outError = structName + ": unknown array length '" + len + "'";
                    return false;
                }
                f.ArrayLen = it->second;
            }
        }
        out.push_back(std::move(f));
    }
    if (out.empty())
        outError = structName + ": no field declarations parsed";
    return !out.empty();
}

bool Std430ScalarOrVector(const std::string& type, uint32_t& size, uint32_t& align)
{
    if (type == "float" || type == "uint" || type == "int" || type == "bool")
    { size = 4; align = 4; return true; }
    if (type == "vec2" || type == "uvec2" || type == "ivec2") { size = 8; align = 8; return true; }
    if (type == "vec3" || type == "uvec3" || type == "ivec3") { size = 12; align = 16; return true; }
    if (type == "vec4" || type == "uvec4" || type == "ivec4") { size = 16; align = 16; return true; }
    return false;
}

uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1u) / a * a; }

// std430: member offsets round up to the member's alignment; an ARRAY's stride is the element
// size rounded to the element alignment (no vec4 round-up — that is std140); the struct's own
// alignment is the max member alignment and its size rounds up to that.
bool ComputeStd430(const std::vector<GlslField>& fields,
                   const std::map<std::string, Std430Layout>& knownStructs, Std430Layout& out,
                   std::string& outError)
{
    out = {};
    uint32_t cursor = 0;
    uint32_t structAlign = 1;
    for (const GlslField& f : fields)
    {
        uint32_t size = 0;
        uint32_t align = 0;
        if (!Std430ScalarOrVector(f.Type, size, align))
        {
            const auto it = knownStructs.find(f.Type);
            if (it == knownStructs.end())
            {
                outError = "unknown member type '" + f.Type + "' (member " + f.Name + ")";
                return false;
            }
            size = it->second.Size;
            align = it->second.Align;
        }
        const uint32_t offset = RoundUp(cursor, align);
        out.Offsets.push_back(offset);
        cursor = offset + (f.IsArray ? RoundUp(size, align) * f.ArrayLen : size);
        structAlign = std::max(structAlign, align);
    }
    out.Align = structAlign;
    out.Size = RoundUp(cursor, structAlign);
    return true;
}

// One expected field of a C++ mirror struct: the GLSL spelling it must carry, and the byte
// offset offsetof() reports on the C++ side.
struct ExpectedField
{
    const char* Name;
    const char* GlslType;
    uint32_t ArrayLen;
    size_t Offset;
};

// One shader file that re-declares a shared std430 struct against the buffer it rides on.
struct MirrorSource
{
    std::string Label;
    std::string Path;
};

#if defined(CBT_SHADER_SOURCE_DIR) && defined(TERRAIN_GRASS_SHADER_SOURCE_DIR) && \
    defined(RENDERING_SHADER_INCLUDES_DIR)
// The guarded set. TerrainParamsEntryMirrorSetIsComplete rediscovers the real set from the tree and
// fails if this list has fallen behind, so adding a fifth mirror cannot go unnoticed.
std::vector<MirrorSource> TerrainParamsMirrorSources()
{
    const std::string cbt = CBT_SHADER_SOURCE_DIR;
    const std::string grass = TERRAIN_GRASS_SHADER_SOURCE_DIR;
    return {
        {"cbt_surface.glsl", cbt + "/cbt_surface.glsl"},
        {"terrain_grass_surface.glsl", grass + "/terrain_grass_surface.glsl"},
        {"terrain_grass_vertex_modifier.glsl", grass + "/terrain_grass_vertex_modifier.glsl"},
        {"terrain_grass_place.comp", grass + "/terrain_grass_place.comp"},
        {"terrain_grass_plan.comp", grass + "/terrain_grass_plan.comp"},
        {"terrain_grass_classify.comp", grass + "/terrain_grass_classify.comp"},
    };
}

// The material record is mirrored against the same table for the same reason, and it is now
// declared ONCE: both terrain surfaces include Includes/terrain_material_albedo.glsl, which carries
// the record and the albedo resolve run on it. The drift lock stays regardless — the mirror is
// against the C++ TerrainMaterialRecord, and one GLSL copy can drift from C++ just as two could —
// and the completeness sweep below is what makes "declared once" a checked fact rather than a
// claim: a file that re-declares the struct fails it.
std::vector<MirrorSource> TerrainMaterialRecordMirrorSources()
{
    const std::string includes = RENDERING_SHADER_INCLUDES_DIR;
    return {
        {"terrain_material_albedo.glsl", includes + "/terrain_material_albedo.glsl"},
    };
}

// The two surfaces that SHADE a terrain. A different set from the mirror list above, and kept
// separate on purpose: "where the record is declared" collapsed to one file when the albedo resolve
// was shared, while "who reads a material" is still two, and the locks below are about the readers.
// Folding them into one list is what makes a lock quietly stop checking one of its two subjects.
std::vector<MirrorSource> TerrainMaterialSurfaces()
{
    const std::string cbt = CBT_SHADER_SOURCE_DIR;
    const std::string grass = TERRAIN_GRASS_SHADER_SOURCE_DIR;
    return {
        {"cbt_surface.glsl", cbt + "/cbt_surface.glsl"},
        {"terrain_grass_surface.glsl", grass + "/terrain_grass_surface.glsl"},
    };
}
#endif

#if defined(GE_ENGINE_MODULES_DIR)
// Every shader under Engine/Modules that declares `struct <name> {`, by filename. The rediscovery
// half of a drift lock: a hand-written mirror list is exactly the thing that goes quietly stale.
std::vector<std::string> DiscoverGlslStructDeclarations(const std::string& structName)
{
    std::vector<std::string> discovered;
    const std::filesystem::path root{GE_ENGINE_MODULES_DIR};
    if (!std::filesystem::is_directory(root))
        return discovered;

    const std::regex declPattern{"struct\\s+" + structName + "\\s*\\{"};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".glsl" && ext != ".comp" && ext != ".vert" && ext != ".frag")
            continue;
        const std::string src = StripLineComments(SlurpFile(entry.path().string()));
        if (std::regex_search(src, declPattern))
            discovered.push_back(entry.path().filename().string());
    }
    std::sort(discovered.begin(), discovered.end());
    return discovered;
}

std::vector<std::string> MirrorLabels(const std::vector<MirrorSource>& sources)
{
    std::vector<std::string> labels;
    for (const auto& source : sources)
        labels.push_back(source.Label);
    std::sort(labels.begin(), labels.end());
    return labels;
}
#endif

} // namespace

TEST(CBTLayout, StructSizesMatchStd430)
{
    EXPECT_EQ(sizeof(CBTNeighbors), 16u);
    EXPECT_EQ(sizeof(CBTBisectorData), 32u);
    // S2a (sector, local) storage: 4 x vec4 legacy block + 4 x uvec2 sector tail. The legacy
    // corner/meta offsets are pinned unmoved so every flag-off consumer stays byte-identical.
    EXPECT_EQ(sizeof(CBTVertexData), 96u);
    EXPECT_EQ(offsetof(CBTVertexData, Meta), 48u);
    EXPECT_EQ(offsetof(CBTVertexData, Sector0), 64u);
    EXPECT_EQ(offsetof(CBTVertexData, DeepTag), 88u);
    // The scalar vec4 block (mat4 + 13 vec4 = 256B, the water plane last) is followed by the
    // analytic sphere-modifier set (1 count vec4 + 16 x 3 vec4 placements) and then the per-cell
    // placement-cull mask tail (48 uvec4 = 768B). Every field before the water plane keeps its
    // offset (the flag-off GLSL reads are byte-stable) — locked below.
    EXPECT_EQ(sizeof(CBTFrameParams), 1808u); // mat4 + 13 vec4 + analytic set + cell masks
    EXPECT_EQ(offsetof(CBTFrameParams, PriorityParams), 224u); // last field before the water plane
    EXPECT_EQ(offsetof(CBTFrameParams, WaterPlane), 240u);
    EXPECT_EQ(offsetof(CBTFrameParams, AnalyticParams), 256u); // appended after the scalar block
    EXPECT_EQ(offsetof(CBTFrameParams, SphereAnalytic), 272u); // vec4-aligned array (std140 stride 48)
    EXPECT_EQ(offsetof(CBTFrameParams, AnalyticCellMask), 1040u); // S3 mask tail (uvec4[48] twin)
    EXPECT_EQ(offsetof(CBTBisectorData, Indices), 20u);
}

// The surface-params buffer is CBT domain state only: the material palette that used to ride in it
// now lives in the terrain material table, which both terrain surfaces and the grass read. Lock
// what remains — the analytic sphere-modifier tail's offsets (leading fields unmoved is what keeps
// the GLSL mirror's reads byte-stable), that the struct still fits one ring slot, and the negative
// that no compiled-in palette came back. Field-by-field C++ <-> GLSL agreement is
// GlslSurfaceStructsMatchCppStd430's job.
TEST(CBTLayout, SurfaceParamsCarriesNoMaterialPalette)
{
    // Dropping the 4 x 48B palette pulled the analytic tail back to the leading block's end
    // (88 + 4 + 4 = 96, still 16-aligned for the std430 vec4-struct array).
    EXPECT_EQ(sizeof(CBTSurfaceParams), 864u);
    EXPECT_EQ(offsetof(CBTSurfaceParams, SphereAnalyticCount), 88u);
    EXPECT_EQ(offsetof(CBTSurfaceParams, SphereAnalytic), 96u); // 16-aligned (std430 vec4 structs)
    EXPECT_LE(sizeof(CBTSurfaceParams), kCBTSurfaceParamsSlotStride); // still fits one ring slot

#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();
    EXPECT_FALSE(std::regex_search(src, std::regex(R"(vec3\s+kLayerColors\s*\[)")))
        << "cbt_surface.glsl declares a compiled-in colour palette again; materials come from the "
           "terrain material table";
    EXPECT_FALSE(std::regex_search(src, std::regex(R"(struct\s+CBTMaterialLayerData\s*\{)")))
        << "cbt_surface.glsl still declares CBTMaterialLayerData, whose C++ twin is deleted — the "
           "mirror would be locked to nothing";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// A blade's grounded base takes the terrain's albedo from the SHARED resolve, not from a second one
// of its own.
//
// This is a drift lock on a defect that shipped. The grass surface used to carry its own
// sampleTerrainMaterialColor: it read the same material RECORD off the same table, which is why it
// looked correct, and then evaluated a different expression on it — the flat tint, or a single
// plain-REPEAT XZ tap, with none of the value-noise variation, hex tiling, side projections or
// world-anchoring UV phase the ground applies. On the shipped untextured palette that pinned every
// blade base to the record's flat tint while the ground beside it carried the variation's brightness
// AND saturation jitter, so the base sat off its own ground by an amount that varies with position
// and is largest in the channel furthest from the tint's luminance — measured at 4.4% in blue and
// 1.3% in red at a close pose, and bounded by the full VariationStrength (0.16 on the grass role).
//
// Reading the same record is NOT the invariant; evaluating the same expression on it is. So this
// asserts the call, and asserts the absence of the two shapes a re-derivation takes: a tap on the
// record's albedo texture, and a tint assembled out of its three colour fields. Either one is a
// second definition of the ground's colour, whatever it is named.
TEST(CBTLayout, GrassGroundsBladesOnTheSharedMaterialAlbedoResolve)
{
#if defined(TERRAIN_GRASS_SHADER_SOURCE_DIR)
    const std::string src = SlurpFile(std::string(TERRAIN_GRASS_SHADER_SOURCE_DIR) +
                                      "/terrain_grass_surface.glsl");
    ASSERT_FALSE(src.empty()) << "cannot read terrain_grass_surface.glsl";

    EXPECT_TRUE(std::regex_search(
        src, std::regex(R"(#include\s+"Includes/terrain_material_albedo\.glsl")")))
        << "the grass surface no longer includes the shared terrain albedo resolve";
    EXPECT_TRUE(std::regex_search(src, std::regex(R"(CBT_MaterialAlbedo\s*\()")))
        << "the grass surface no longer calls CBT_MaterialAlbedo, so a blade's base is being "
           "coloured by something other than the ground's own albedo expression";

    const std::string body = StripLineComments(src);
    EXPECT_FALSE(std::regex_search(body, std::regex(R"(texture(Lod)?\s*\(\s*GE_BTEX\s*\(\s*mat\.)")))
        << "the grass surface samples a material record's texture directly again — that is a second "
           "resolve of the terrain's albedo, which is the drift this test exists to stop";
    EXPECT_FALSE(std::regex_search(body, std::regex(R"(vec3\s*\(\s*mat\.AlbedoR)")))
        << "the grass surface assembles a material tint itself again; the tint is only the terrain's "
           "colour after the shared resolve has run the variation on it";
#else
    GTEST_SKIP() << "TERRAIN_GRASS_SHADER_SOURCE_DIR not defined";
#endif
}

// The shipped blend width. Every surviving material costs a full set of taps — albedo, normal and
// ORM, each up to three triplanar projections and each up to three more under hex tiling — so this
// constant is very nearly a direct multiplier on terrain fragment texture traffic.
//
// THE BOUNDARY. 3 is where a four-channel splat stops being approximated: at or below three active
// materials nothing is dropped and the fragment is bitwise the full blend, so only the corners where
// all four are painted together differ from the reference at all. Dropping to 2 makes every
// three-material band an approximation as well and saves a measured 0.204 ms at texel range; going
// to 4 removes the cap entirely and with it the budget this constant exists to be.
//
// Pinned to an exact value rather than range-checked, in both directions: raising it spends frame
// time that is invisible in a diff and only measurable in Release, and lowering it trades away
// reference-identical shading over most of a painted terrain. Either is a decision, not a tweak.
//
// The compile side is covered elsewhere: ShippedSurfaceCompose.CbtTerrainSurfaceComposesAtEveryBlendWidth
// builds the surface at 2, 3 and 4, so this test pins WHICH width ships, not whether others work.
TEST(CBTLayout, SurfaceBlendsThreeMaterialsPerFragment)
{
#if defined(RENDERING_SHADER_INCLUDES_DIR)
    // Comments stripped first, as the mirror tests do: this constant is discussed at length in the
    // block above it, and a commented-out or illustrative value must not be able to satisfy the pin.
    const std::string src = StripLineComments(
        SlurpFile(std::string(RENDERING_SHADER_INCLUDES_DIR) + "/terrain_blend_width.glsl"));
    ASSERT_FALSE(src.empty()) << "cannot read terrain_blend_width.glsl";

    std::smatch match;
    ASSERT_TRUE(std::regex_search(
        src, match, std::regex(R"(#define[ \t]+CBT_MAX_BLEND_MATERIALS[ \t]+(\d+))")))
        << "terrain_blend_width.glsl no longer defines CBT_MAX_BLEND_MATERIALS; the per-fragment "
           "material count is unbounded again";
    EXPECT_EQ(match[1].str(), "3")
        << "the terrain surfaces ship blending " << match[1].str()
        << " materials per fragment instead of 3 — each material is another albedo + normal + ORM "
           "tap set per pixel, and below 3 a plain three-material band stops being reference-exact";
#else
    GTEST_SKIP() << "RENDERING_SHADER_INCLUDES_DIR not defined";
#endif
}

// Neither terrain surface may keep its own copy of the width.
//
// The resolve is parameterized by a width its INCLUDER defines — the only way the GLSL preprocessor
// can hand a constant to an include — so nothing stops a surface from spelling out its own value
// and drifting from the other. A blade tints its base with the ground colour beneath it, so a grass
// copy left behind at a different width shades blades against a blend the ground stopped using:
// visible only where four materials meet or one material is named by two channels, on wind-moving
// geometry, in the one place a screenshot is hardest to compare. Nothing else fails — both shaders
// still compile, and every test that executes the resolve executes it at a width IT chose.
//
// One shared definition removes the drift rather than policing it, and this holds it that way: the
// pin above is only the SHIPPED value if both surfaces actually take their width from that file.
TEST(CBTLayout, NeitherTerrainSurfaceDefinesItsOwnBlendWidth)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(TERRAIN_GRASS_SHADER_SOURCE_DIR) && \
    defined(RENDERING_SHADER_INCLUDES_DIR)
    const std::vector<MirrorSource> surfaces = TerrainMaterialSurfaces();
    ASSERT_EQ(surfaces.size(), 2u) << "the set of material-reading surfaces changed";

    for (const MirrorSource& surface : surfaces)
    {
        // Comments stripped first, as the pin above does: an illustrative or commented-out width
        // must not be able to break this lock.
        const std::string src = StripLineComments(SlurpFile(surface.Path));
        ASSERT_FALSE(src.empty()) << "cannot read " << surface.Path;

        std::smatch match;
        EXPECT_FALSE(std::regex_search(
            src, match, std::regex(R"(#define[ \t]+CBT_MAX_BLEND_MATERIALS[ \t]+(\d+))")))
            << surface.Label
            << " defines its own CBT_MAX_BLEND_MATERIALS instead of including "
               "Includes/terrain_blend_width.glsl — a second copy of the knob, which is how the two "
               "surfaces come to blend different numbers of materials";
        EXPECT_NE(src.find("#include \"Includes/terrain_blend_width.glsl\""), std::string::npos)
            << surface.Label << " does not include the shared blend width";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR / TERRAIN_GRASS_SHADER_SOURCE_DIR not defined";
#endif
}

// Both terrain surfaces must actually REACH the shared resolve. Taking the shared width says
// nothing about using the shared ranking: a surface that included the width and then blended all
// four channels itself would compile, and would pass the lock above, while diverging from the
// ground exactly as it did before this include existed.
TEST(CBTLayout, BothTerrainSurfacesIncludeTheSharedBlendResolve)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(TERRAIN_GRASS_SHADER_SOURCE_DIR) && \
    defined(RENDERING_SHADER_INCLUDES_DIR)
    const std::vector<MirrorSource> surfaces = TerrainMaterialSurfaces();
    ASSERT_EQ(surfaces.size(), 2u) << "the set of material-reading surfaces changed";

    for (const MirrorSource& surface : surfaces)
    {
        const std::string src = StripLineComments(SlurpFile(surface.Path));
        ASSERT_FALSE(src.empty()) << "cannot read " << surface.Path;
        EXPECT_NE(src.find("#include \"Includes/terrain_blend_resolve.glsl\""), std::string::npos)
            << surface.Label << " no longer includes the shared blend resolve";
        // The resolve is the composition of the two steps; calling either one directly at the call
        // site is how the fold-before-rank order gets re-decided per surface.
        EXPECT_NE(src.find("CBT_ResolveBlendWeights("), std::string::npos)
            << surface.Label << " includes the shared resolve but never calls it";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR / TERRAIN_GRASS_SHADER_SOURCE_DIR not defined";
#endif
}

// A newly-resident atlas tile does not snap to full detail: the row carries a Fade that ramps over
// kAtlasUpgradeFadeSeconds, and the ground mixes coarse->slot by it so a streaming upgrade reads as
// a fade. The blades standing on that ground take their base colour from the same tile, so they
// have to travel with it — otherwise a blade reaches full detail while the surface it grows out of
// is still halfway there, for the whole length of the window.
//
// Locked as a PAIR rather than one file at a time, because the defect is a DISAGREEMENT. Either
// side dropping its Fade read is the same bug, and a per-file test would happily watch the ground
// stop fading while the grass mirror went on faithfully mirroring nothing.
TEST(CBTLayout, BothTerrainSurfacesCrossfadeANewlyResidentSlotInFromTheCoarseField)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(TERRAIN_GRASS_SHADER_SOURCE_DIR)
    const std::string ground =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl"));
    ASSERT_FALSE(ground.empty()) << "cannot read cbt_surface.glsl";

    // The ground crossfades exactly two fields on the row Fade — its atlas NORMAL and its atlas
    // SPLAT — and the grass surface mirrors both. A third would be a new field the blades have to
    // decide about too, so it belongs here as a prompt rather than passing unnoticed.
    size_t groundFades = 0;
    for (size_t at = ground.find("r.Fade < 1.0"); at != std::string::npos;
         at = ground.find("r.Fade < 1.0", at + 1))
        ++groundFades;
    EXPECT_EQ(groundFades, 2u)
        << "cbt_surface.glsl gates " << groundFades
        << " crossfades on the row Fade, expected 2 (atlas normal + atlas splat). The grass surface "
           "mirrors this set; a change here is a decision for the blades as well";

    const std::string grass = StripLineComments(
        SlurpFile(std::string(TERRAIN_GRASS_SHADER_SOURCE_DIR) + "/terrain_grass_surface.glsl"));
    const std::string shared = StripLineComments(
        SlurpFile(std::string(TERRAIN_GRASS_SHADER_SOURCE_DIR) + "/grass_atlas_splat.glsl"));
    ASSERT_FALSE(grass.empty()) << "cannot read terrain_grass_surface.glsl";
    ASSERT_FALSE(shared.empty()) << "cannot read grass_atlas_splat.glsl";

    // One rule for both grass consumers, owned by the shared include, and driven by the row the
    // grass resolve itself read — not by a clock of its own.
    EXPECT_NE(shared.find("float GrassAtlas_SlotUpgradeWeight("), std::string::npos)
        << "the shared grass include no longer owns the upgrade-crossfade weight; a copy per "
           "consumer is how the blade and the ground come to disagree about when a tile is settled";
    EXPECT_NE(shared.find("r.Fade"), std::string::npos)
        << "the shared grass resolve computes an upgrade weight without reading the resolved row's "
           "Fade — a second clock rather than the ground's";
    EXPECT_NE(grass.find("GrassAtlas_SlotUpgradeWeight("), std::string::npos)
        << "terrain_grass_surface.glsl takes the atlas normal outright, so the triplanar projection "
           "weights under a blade would step on upgrade while the ground beside it faded";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR / TERRAIN_GRASS_SHADER_SOURCE_DIR not defined";
#endif
}

// Flag bits are the one part of the record the GLSL mirror carries as a LITERAL rather than as a
// parsed field, so the struct-layout lock above cannot see them drift. The surface gates its
// metallic read on this bit: a mirror that drifted would either shade every ORM material as a black
// mirror or never read metal at all, and both look like content problems from the outside.
TEST(CBTLayout, GlslMirrorsTheOrmMetallicFlagBit)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string src =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl"));
    ASSERT_FALSE(src.empty()) << "cannot read cbt_surface.glsl";

    std::smatch m;
    ASSERT_TRUE(std::regex_search(
        src, m, std::regex(R"(const\s+uint\s+CBT_MATFLAG_ORM_HAS_METALLIC\s*=\s*([0-9]+)u?\s*;)")))
        << "cbt_surface.glsl no longer declares CBT_MATFLAG_ORM_HAS_METALLIC";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kTerrainMaterialFlagOrmHasMetallic)
        << "the GLSL metallic flag bit drifted from kTerrainMaterialFlagOrmHasMetallic";

    // And the surface must actually gate on it — a declared-but-unread constant is the state this
    // whole flag was in before the ORM sampling landed.
    EXPECT_TRUE(std::regex_search(src, std::regex(R"(Flags\s*&\s*CBT_MATFLAG_ORM_HAS_METALLIC)")))
        << "cbt_surface.glsl declares the metallic flag but never tests it";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The Planar projection bit, mirrored as a literal in the shared albedo include: a drift either
// projects every orthophoto triplanar again or flattens a tiling material onto the footprint.
TEST(CBTLayout, GlslMirrorsThePlanarProjectionFlagBit)
{
#if defined(RENDERING_SHADER_INCLUDES_DIR)
    const std::string src = StripLineComments(
        SlurpFile(std::string(RENDERING_SHADER_INCLUDES_DIR) + "/terrain_material_albedo.glsl"));
    ASSERT_FALSE(src.empty()) << "cannot read terrain_material_albedo.glsl";

    std::smatch m;
    ASSERT_TRUE(std::regex_search(
        src, m, std::regex(R"(const\s+uint\s+CBT_MATFLAG_PLANAR\s*=\s*([0-9]+)u?\s*;)")))
        << "terrain_material_albedo.glsl no longer declares CBT_MATFLAG_PLANAR";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kTerrainMaterialFlagPlanar)
        << "the GLSL planar flag bit drifted from kTerrainMaterialFlagPlanar";
    EXPECT_TRUE(std::regex_search(src, std::regex(R"(Flags\s*&\s*CBT_MATFLAG_PLANAR)")))
        << "terrain_material_albedo.glsl declares the planar flag but never tests it";
#else
    GTEST_SKIP() << "RENDERING_SHADER_INCLUDES_DIR not defined";
#endif
}

// What Planar MEANS, read from the shipped source: the material's albedo, normal and ORM each take
// one tap at uv.Planar when the flag is set, and uv.Planar is derived from the terrain's footprint
// UV, not from the world position the triplanar sets use. The footprint UV must reach the context
// from both surfaces: the ground passes sIn.uv0, and the grass passes the footprint UV it derives
// under the blade, so a blade's base keeps landing on the colour of the ground beside it.
//
// THE V DIRECTION. CBT_TerrainToWorldXZ maps the footprint UV's y to world +Z, the world is +Z
// north, and an image file stores its north row first. So uv.Planar must reverse V (row 0 on the
// largest-Z edge) or every north-up orthophoto lands mirrored north to south, which looks like a
// plausible terrain and is wrong everywhere off the east-west center line. Its gradients reverse
// with it, and the normal arm, which folds on the Y-facing projection's frame, must negate tangent V
// for the same reason: a +t.y there lights every Planar bump from the wrong side.
//
// A planet's uv0 is a cube-face UV, so the ground clears the Planar bit on a sphere and the
// material shades Triplanar there rather than repeating per face.
TEST(CBTLayout, PlanarMaterialsTapEveryMapOnceAtTheFootprintUV)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(RENDERING_SHADER_INCLUDES_DIR) && \
    defined(TERRAIN_GRASS_SHADER_SOURCE_DIR)
    const std::string albedo = StripLineComments(
        SlurpFile(std::string(RENDERING_SHADER_INCLUDES_DIR) + "/terrain_material_albedo.glsl"));
    const std::string surface =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl"));
    const std::string layout =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl"));
    const std::string grass = StripLineComments(SlurpFile(
        std::string(TERRAIN_GRASS_SHADER_SOURCE_DIR) + "/terrain_grass_surface.glsl"));
    ASSERT_FALSE(albedo.empty());
    ASSERT_FALSE(surface.empty());
    ASSERT_FALSE(layout.empty());
    ASSERT_FALSE(grass.empty());

    EXPECT_TRUE(std::regex_search(
        albedo, std::regex(R"(footprint\s*=\s*clamp\(\s*tc\.terrainUV\s*\*\s*footprintScale\s*,\s*vec2\(0\.0\)\s*,\s*vec2\(1\.0\)\s*\))")))
        << "CBT_MaterialUVs no longer derives the Planar footprint from the terrain's footprint UV, "
           "clamped to the authored size";
    EXPECT_TRUE(std::regex_search(
        albedo, std::regex(R"(uv\.Planar\s*=\s*vec2\(\s*footprint\.x\s*,\s*1\.0\s*-\s*footprint\.y\s*\)\s*\*\s*planarTiling)")))
        << "uv.Planar no longer puts image row 0 on the north (largest-Z) edge; a north-up "
           "orthophoto lands mirrored";
    EXPECT_TRUE(std::regex_search(
        albedo, std::regex(R"(planarRate\s*=\s*vec2\(\s*footprintScale\.x\s*,\s*-\s*footprintScale\.y\s*\)\s*\*\s*planarTiling)")))
        << "the Planar gradients no longer reverse V with the UV they belong to";
    EXPECT_TRUE(std::regex_search(
        albedo, std::regex(R"(if\s*\(\s*CBT_MatIsPlanar\(mat\)\s*\)\s*return\s+CBT_PlanarTap\(slot,\s*mat\.AlbedoTex,\s*mat\.HexRotStrength,\s*uv\s*\))")))
        << "CBT_MaterialAlbedo does not take a Planar material's albedo at uv.Planar";
    EXPECT_TRUE(std::regex_search(
        surface, std::regex(R"(if\s*\(\s*CBT_MatIsPlanar\(mat\)\s*\)\s*return\s+CBT_PlanarTap\(slot,\s*mat\.OrmTex,\s*mat\.HexRotStrength,\s*uv\s*\))")))
        << "CBT_MaterialOrm does not take a Planar material's ORM at uv.Planar";

    // The normal arm: decoded at uv.Planar, folded and swizzled on the XZ frame.
    EXPECT_TRUE(std::regex_search(
        surface,
        std::regex(R"(if\s*\(\s*CBT_MatIsPlanar\(mat\)\s*\)\s*\{\s*vec3\s+t\s*=\s*CBT_ProjNormalFromSample\(\s*CBT_PlanarTap\(\s*CBT_LAYER_SLOT\(layerOrd,\s*1u\),\s*mat\.NormalTex,\s*mat\.HexRotStrength,\s*uv\s*\)\s*,[^;]*;\s*return\s+vec3\(\s*vec2\(\s*t\.x\s*,\s*-\s*t\.y\s*\)\s*\+\s*n\.xz\s*,\s*abs\(t\.z\)\s*\*\s*n\.y\)\.xzy\s*;)")))
        << "CBT_MaterialNormal's Planar arm is not the XZ-frame fold with tangent V reversed";
    // EDGE SAMPLING. An image that covers the terrain once is sampled clamp-to-edge: on REPEAT a
    // tap at u or v = 0 or 1 filters the opposite edge in, and the footprint clamp puts every
    // overhang sample exactly there. Both surfaces must define it on the ANISOTROPIC clamp preset.
    EXPECT_TRUE(std::regex_search(
        albedo, std::regex(R"(uv\.PlanarEdgeClamp\s*=\s*planarTiling\s*<=\s*1\.0\s*;)")))
        << "a Planar image at Tiling 1 no longer selects clamp-to-edge sampling";
    EXPECT_TRUE(std::regex_search(
        albedo, std::regex(R"(if\s*\(\s*uv\.PlanarEdgeClamp\s*\)\s*return\s+CBT_LAYER_TAP_EDGE\(\s*slot\s*,\s*tex\s*,\s*uv\.Planar\s*,)")))
        << "CBT_PlanarTap no longer takes the clamp-to-edge tap for an image that covers the terrain once";
    for (const std::string* src : {&surface, &grass})
        EXPECT_TRUE(std::regex_search(
            *src, std::regex(R"(#define\s+CBT_LAYER_TEX_EDGE\(texIdx\)[^\n]*GE_TS_CLAMP_ANISO\b)")))
            << "a terrain surface's CBT_LAYER_TEX_EDGE is not the anisotropic clamp-to-edge preset "
               "(GE_TS_CLAMP_ANISO); the isotropic GE_TS_CLAMP blurs the image at grazing angles";
    EXPECT_TRUE(std::regex_search(
        surface, std::regex(R"(if\s*\(\s*isSphere\s*\)\s*mat\.Flags\s*&=\s*~\s*CBT_MATFLAG_PLANAR\s*;)")))
        << "the ground no longer clears the Planar bit on a sphere, where uv0 is a cube-face UV";
    // The frame the arm assumes: the footprint UV's x runs along world X and its y along world Z.
    EXPECT_TRUE(std::regex_search(
        layout, std::regex(R"(vec2\s+CBT_TerrainToWorldXZ\(vec2\s+uv,\s*CBTFrameParams\s+fp\)\s*\{\s*return\s+fp\.terrainOrigin\.xy\s*\+\s*uv\s*\*\s*fp\.terrainSize\.xy\s*;)")))
        << "the footprint UV no longer maps (x, y) to world (X, Z) as written; re-derive the "
           "Planar normal arm's swizzle";

    EXPECT_TRUE(std::regex_search(
        surface, std::regex(R"(CBT_BuildTriplanarCtx\(\s*sIn\.positionRelWS\s*,\s*normalWS\s*,\s*sIn\.uv0\s*\))")))
        << "the ground no longer hands the footprint UV (sIn.uv0) to the material context";
    EXPECT_TRUE(std::regex_search(
        grass, std::regex(R"(CBT_BuildTriplanarCtx\(\s*posRelWS\s*,[^;]*,\s*uv\s*\)\s*;)")))
        << "the grass no longer hands its footprint UV to the material context";
#else
    GTEST_SKIP() << "shader source directories not defined";
#endif
}

// The per-kernel layout filter reads the bindings a cooked kernel declares, and it must read
// them PER GROUP. The cook appends the push-constant block as `@group(3) @binding(0)`, so a
// group-blind scan reports a binding 0 for every kernel that takes push constants — and binding
// 0 of the compute set is the HeapID SSBO. That silently hands kernels a storage buffer they
// never declared, spending one of the ten slots WebGPU guarantees per compute stage, which is
// the exact budget this filtering exists to protect.
TEST(CBTLayout, DeclaredBindingsIgnoresOtherDescriptorGroups)
{
    // The shape the cook emits: set-0 storage buffers plus the group-3 push-constant block.
    const char* kWgsl =
        "@group(0) @binding(5)\n"
        "var<storage, read_write> global: type_9;\n"
        "@group(3) @binding(0)\n"
        "var<uniform> global_1: type_16;\n"
        "@group(0) @binding(6)\n"
        "var<storage, read_write> global_2: type_13;\n";
    const std::vector<uint8_t> bytes(kWgsl, kWgsl + std::strlen(kWgsl));

    const std::vector<uint32_t> set0 = DeclaredBindings(bytes, 0u);
    EXPECT_EQ(set0.size(), 2u) << "set 0 declares exactly bindings 5 and 6 here";
    EXPECT_NE(std::find(set0.begin(), set0.end(), 5u), set0.end());
    EXPECT_NE(std::find(set0.begin(), set0.end(), 6u), set0.end());
    EXPECT_EQ(std::find(set0.begin(), set0.end(), 0u), set0.end())
        << "the group-3 push-constant binding leaked into the set-0 list — that is the HeapID "
           "SSBO slot, granted to a kernel that never declared it";

    // And the push-constant group itself still reads back, so the parser is group-selective
    // rather than merely filtering zero out.
    const std::vector<uint32_t> set3 = DeclaredBindings(bytes, 3u);
    ASSERT_EQ(set3.size(), 1u);
    EXPECT_EQ(set3[0], 0u);
}

// Texture PRESENCE bits. The compat profile resolves every bindless index to the unbound
// sentinel, so an index test reads "untextured" for every layer there and the surface returns its
// tint before taking a tap — a bound texture that never renders. These flags are the presence
// answer, and this pins the GLSL mirrors (both surfaces) to the C++ bits and pins that each one
// is actually TESTED, which is exactly the state the metallic flag was caught in above.
TEST(CBTLayout, GlslMirrorsTheTexturePresenceFlagBits)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string src =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl"));
    ASSERT_FALSE(src.empty()) << "cannot read cbt_surface.glsl";

    const struct
    {
        const char* name;
        uint32_t expected;
    } kBits[] = {
        {"CBT_MATFLAG_HAS_ALBEDO", kTerrainMaterialFlagHasAlbedo},
        {"CBT_MATFLAG_HAS_NORMAL", kTerrainMaterialFlagHasNormal},
        {"CBT_MATFLAG_HAS_ORM", kTerrainMaterialFlagHasOrm},
    };
    for (const auto& bit : kBits)
    {
        std::smatch m;
        const std::string pattern =
            std::string(R"(const\s+uint\s+)") + bit.name + R"(\s*=\s*([0-9]+)u?\s*;)";
        ASSERT_TRUE(std::regex_search(src, m, std::regex(pattern)))
            << "cbt_surface.glsl no longer declares " << bit.name;
        EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), bit.expected)
            << "the GLSL presence bit drifted from the C++ value for " << bit.name;
        EXPECT_TRUE(std::regex_search(src, std::regex(std::string("Flags\\s*&\\s*") + bit.name)))
            << bit.name << " is declared but never tested";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// PROJECTION AXES of the triplanar normal blend, derived rather than transcribed.
//
// CBT's terrain mesh carries no vertex tangents: each projection's tangent frame IS its world axis
// pair, and CBT_MaterialUVs is what declares that pair (uv.XZ = pos.xz => U is world X, V is world
// Z). CBT_MaterialNormal then folds the base normal in on those same two axes and swizzles the
// result back to world orientation. The two functions have to agree, and nothing else can see it
// when they stop: a wrong swizzle still compiles, still renders, and only lights detail from the
// wrong side.
//
// So this reads each projection's axis pair out of CBT_MaterialUVs and DERIVES the swizzle that
// projection must carry, rather than asserting the swizzle the shader happens to have — a
// transcribed expectation would pass by construction and pin nothing. Concretely: this shader
// projects the X-facing plane as pos.yz, so its swizzle must be .zxy; triplanar_pbr.glsl projects
// that plane as pos.zy and correctly uses .zyx, and copying that here would swap this projection's
// world Y and Z and tilt cliff detail the wrong way.
//
// WHAT THIS DOES NOT PIN: the SIGN of tangent Y. A globally inverted green channel is
// self-consistent across all three projections and satisfies every assertion below while rendering
// each authored bump as a dent, and a flip written into CBT_DecodeProjNormal instead of into the
// swizzle never reaches these patterns at all. The sign is pinned by CBTProjNormalSignTests.cpp,
// which executes the extracted decode instead of reading it. The convention it encodes is the
// standard_pbr mesh path's: the same normal map must light an authored bump the same way on
// terrain and on a mesh. A change to the sign convention has to be re-verified against the mesh
// path at runtime, not here.
TEST(CBTLayout, TriplanarNormalSwizzlesFollowTheProjectionAxesTheUVsDeclare)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(RENDERING_SHADER_INCLUDES_DIR)
    // The two halves of the agreement now live in two files: CBT_MaterialUVs declares the axis
    // pairs from the shared albedo include, and CBT_MaterialNormal swizzles against them in
    // cbt_surface.glsl. Both are read, because the point of the test is that they agree.
    const std::string uvSrc = StripLineComments(
        SlurpFile(std::string(RENDERING_SHADER_INCLUDES_DIR) + "/terrain_material_albedo.glsl"));
    const std::string normalSrc =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl"));
    ASSERT_FALSE(uvSrc.empty()) << "cannot read terrain_material_albedo.glsl";
    ASSERT_FALSE(normalSrc.empty()) << "cannot read cbt_surface.glsl";
    const std::string src = uvSrc + "\n" + normalSrc;

    // World component index of an axis letter, so a swizzle can be indexed by world axis.
    const auto axisIndex = [](char c) { return c == 'x' ? 0 : (c == 'y' ? 1 : 2); };

    struct Projection
    {
        const char* Member; // CBTMaterialUV member: this projection's UV set
        const char* Plane;  // for failure messages
    };
    const Projection kProjections[] = {
        {"XY", "Z-facing"},
        {"XZ", "Y-facing (flat ground's single tap)"},
        {"YZ", "X-facing"},
    };

    for (const Projection& proj : kProjections)
    {
        // 1. Which world axes does this projection's UV set actually span?
        std::smatch uvMatch;
        const std::regex uvRe(R"(uv\.)" + std::string(proj.Member) +
                              R"(\s*=\s*tc\.pos\.([xyz])([xyz]))");
        ASSERT_TRUE(std::regex_search(src, uvMatch, uvRe))
            << "CBT_MaterialUVs no longer derives uv." << proj.Member
            << " from tc.pos; the handedness derivation has no axis pair to work from";
        const char uAxis = uvMatch[1].str()[0];
        const char vAxis = uvMatch[2].str()[0];
        // The projection's own axis is the one its UVs do not span.
        char nAxis = 'x';
        for (const char c : {'x', 'y', 'z'})
        {
            if (c != uAxis && c != vAxis)
                nAxis = c;
        }

        // 2. How does CBT_MaterialNormal fold and swizzle that same projection?
        std::smatch nMatch;
        // `[^;]*` spans whatever else the decode call carries after the UV set — the compat
        // profile's explicit gradients ride there. It stops at the call's own semicolon, so the
        // pairing this test derives (this projection's UV set to this projection's swizzle) is
        // still the thing being matched; only the argument list is allowed to grow.
        const std::regex nRe(
            R"(uv\.)" + std::string(proj.Member) +
            R"([^;]*\);\s*acc\s*\+=\s*w\.([xyz])\s*\*\s*vec3\(t\.xy\s*\+\s*n\.([xyz])([xyz])\s*,\s*abs\(t\.z\)\s*\*\s*n\.([xyz])\)\.([xyz])([xyz])([xyz])\s*;)");
        ASSERT_TRUE(std::regex_search(src, nMatch, nRe))
            << "CBT_MaterialNormal's " << proj.Plane
            << " block is not in the shape this test can read; re-derive it rather than relaxing "
               "the pattern";

        // 3. The blend weight is indexed by world axis (CBT_TriplanarWeights takes componentwise
        //    powers of the surface normal), so the weight a projection accumulates under is the
        //    axis its UVs do NOT span — its own. Swapping two of them still sums to 1 and still
        //    renders, but weights each plane by how strongly the surface faces a DIFFERENT one.
        EXPECT_EQ(nMatch[1].str()[0], nAxis)
            << proj.Plane << ": accumulated under w." << nMatch[1].str()[0] << ", but uv."
            << proj.Member << " spans " << uAxis << vAxis << " so this plane's own axis is "
            << nAxis;

        // 4. The base normal must fold in on the SAME axes the UVs span. Folding n.xy into a
        //    projection whose UVs are pos.xz mixes two different tangent frames.
        EXPECT_EQ(nMatch[2].str()[0], uAxis)
            << proj.Plane << ": base normal folded on the wrong U axis";
        EXPECT_EQ(nMatch[3].str()[0], vAxis)
            << proj.Plane << ": base normal folded on the wrong V axis";
        EXPECT_EQ(nMatch[4].str()[0], nAxis)
            << proj.Plane << ": tangent Z scaled by the wrong world axis of the base normal";

        // 5. The guard that admits the tap must test the SAME weight the tap accumulates under.
        //    A guard on another axis skips a plane the surface is facing (flat ground loses its
        //    only tap) or admits one it is edge-on to.
        std::smatch gMatch;
        // The leading `[^;]*` spans the decode call's other arguments (the layer ordinal, and the
        // compat profile's gradients after the UV set); it cannot cross the statement's semicolon,
        // so the guard and the UV set it admits still have to be the same call.
        const std::regex gRe(R"(if\s*\(\s*w\.([xyz])\s*>\s*0\.0\s*\)\s*\{\s*vec3\s+t\s*=\s*)"
                             R"(CBT_DecodeProjNormal\([^;]*uv\.)" +
                             std::string(proj.Member) + R"([,\)])");
        ASSERT_TRUE(std::regex_search(src, gMatch, gRe))
            << "CBT_MaterialNormal's " << proj.Plane << " tap is no longer guarded in the shape "
                                                        "this test can read";
        EXPECT_EQ(gMatch[1].str()[0], nAxis)
            << proj.Plane << ": guarded on w." << gMatch[1].str()[0] << " but accumulates this "
            << "plane, whose own axis is " << nAxis;

        // 6. The swizzle routes the pre-swizzle vector (U-contribution, V-contribution,
        //    N-contribution) back to the world axes those contributions belong to. Indexed by
        //    world axis, the swizzle letter is therefore x on the U axis, y on the V axis and z on
        //    the projection axis. Any other arrangement mirrors or rotates this projection.
        const char swizzle[3] = {nMatch[5].str()[0], nMatch[6].str()[0], nMatch[7].str()[0]};
        EXPECT_EQ(swizzle[axisIndex(uAxis)], 'x')
            << proj.Plane << ": world " << uAxis << " does not take the U contribution (uv."
            << proj.Member << " spans " << uAxis << vAxis << ")";
        EXPECT_EQ(swizzle[axisIndex(vAxis)], 'y')
            << proj.Plane << ": world " << vAxis << " does not take the V contribution (uv."
            << proj.Member << " spans " << uAxis << vAxis << ")";
        EXPECT_EQ(swizzle[axisIndex(nAxis)], 'z')
            << proj.Plane << ": world " << nAxis
            << " does not take the surface-normal contribution";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The material TABLE contract: one record type, indexed by slot ID, sized so a full table is a
// trivial upload. Field-by-field GLSL agreement is locked in GlslSurfaceStructsMatchCppStd430;
// here it is the size, the table arithmetic and the "unbound falls back to the scalar" defaults.
TEST(CBTLayout, TerrainMaterialRecordContract)
{
    EXPECT_EQ(sizeof(TerrainMaterialRecord), 80u); // 20 x 4B std430, all-scalar => stride == sizeof
    EXPECT_EQ(sizeof(TerrainMaterialRecord) % 16u, 0u) << "std430 array stride must stay 16-aligned";

    // 8-bit slot IDs, so a terrain's whole table is 20 KiB — cheap enough to rewrite on a render-
    // origin rebase rather than tracking dirty ranges.
    EXPECT_EQ(kMaxTerrainMaterials, 256u);
    EXPECT_EQ(kMaxTerrainMaterials * sizeof(TerrainMaterialRecord), 20480u);
    EXPECT_EQ(kMaxTerrainMaterials - 1u, 255u) << "a slot ID must fit in a uint8";

    // The splat still carries exactly four weights; what changes is what they point at.
    EXPECT_EQ(kTerrainLayerRoleCount, 4u);

    // Flag bits are distinct and single-bit — they are packed into one uint the shader tests.
    EXPECT_EQ(kTerrainMaterialFlagHexTiling, 1u);
    EXPECT_EQ(kTerrainMaterialFlagRetired, 2u);
    EXPECT_EQ(kTerrainMaterialFlagOrmHasMetallic, 4u);

    // A default record is fully UNBOUND: every texture slot is the reserved bindless sentinel, so
    // the fragment shades it from the tint and the paired scalars. A zero-filled table entry (the
    // fallback for an unused slot) must therefore never sample a texture.
    const TerrainMaterialRecord defaultRecord{};
    EXPECT_EQ(defaultRecord.AlbedoTex, kTerrainMaterialUnboundTexture);
    EXPECT_EQ(defaultRecord.NormalTex, kTerrainMaterialUnboundTexture);
    EXPECT_EQ(defaultRecord.OrmTex, kTerrainMaterialUnboundTexture);
    EXPECT_EQ(defaultRecord.Flags, 0u);
    EXPECT_FLOAT_EQ(defaultRecord.Tiling, 1.0f);
    EXPECT_FLOAT_EQ(defaultRecord.VariationStrength, 0.0f);
    EXPECT_FLOAT_EQ(defaultRecord.HexRotStrength, 0.0f);

    // The three paired scalars default to the MULTIPLIER IDENTITY, so a record that binds a map
    // passes it through untrimmed until an author says otherwise. Pinned here because this is the
    // GPU mirror of TerrainMaterialEntry and nothing else can see the two drift: the table tests
    // author every scalar explicitly (AuthorTerrainMaterialRecord overwrites all three), so a
    // struct default that stopped being 1.0 would shade every bound map through a trim nobody
    // asked for and no other test would notice.
    EXPECT_FLOAT_EQ(defaultRecord.Roughness, 1.0f);
    EXPECT_FLOAT_EQ(defaultRecord.Ao, 1.0f);
    EXPECT_FLOAT_EQ(defaultRecord.NormalStrength, 1.0f);
}

// The four default records ARE the untextured terrain palette: the editor swatch, both terrain
// domains and the grass blade colour all resolve here, so these values are what an un-authored
// terrain looks like. Two columns are load-bearing and neither is visible at a glance.
//
// The ROUGHNESS column is the shading these records took over. Until the table shipped, the surface
// blended TerrainGPUParams::LayerRoughness — four scalars nothing ever wrote, so their struct
// defaults were the shipped look. Those four numbers are spelled out below because that source is
// gone: they are no longer derivable from anything, and changing one silently re-shades every
// un-authored terrain in the engine.
TEST(CBTLayout, DefaultTerrainMaterialsCarryTheShippedPalette)
{
    ASSERT_EQ(std::size(kDefaultTerrainMaterials),
              static_cast<size_t>(GameEngine::Terrain::kMaxTerrainMaterialLayers));

    // grass / rock / dirt / snow — linear RGB, the order the splat blends its channels.
    const float kTints[4][3] = {
        {0.35f, 0.55f, 0.18f}, {0.55f, 0.50f, 0.42f}, {0.50f, 0.38f, 0.25f}, {0.90f, 0.92f, 0.95f}};
    // The values TerrainGPUParams::LayerRoughness defaulted to, which the surface blended before it
    // resolved roughness through a record.
    const float kRoughness[4] = {0.85f, 0.65f, 0.75f, 0.90f};

    for (size_t role = 0; role < std::size(kDefaultTerrainMaterials); ++role)
    {
        const TerrainMaterialRecord& material = kDefaultTerrainMaterials[role];
        EXPECT_FLOAT_EQ(material.AlbedoR, kTints[role][0]) << "role " << role;
        EXPECT_FLOAT_EQ(material.AlbedoG, kTints[role][1]) << "role " << role;
        EXPECT_FLOAT_EQ(material.AlbedoB, kTints[role][2]) << "role " << role;
        EXPECT_FLOAT_EQ(material.Roughness, kRoughness[role])
            << "role " << role << " roughness drifted from the shipped untextured look";

        // Unbound: the palette IS the colour until a terrain authors a texture into the slot.
        EXPECT_EQ(material.AlbedoTex, kTerrainMaterialUnboundTexture) << "role " << role;
        EXPECT_FLOAT_EQ(material.Tiling, 1.0f) << "role " << role;
        // Every role carries procedural break-up; a zero scale flattens it to a constant tint,
        // which is the look the variation term exists to kill.
        EXPECT_GT(material.VariationScale, 0.0f) << "role " << role;
        EXPECT_GT(material.VariationStrength, 0.0f) << "role " << role;
    }
}

// The C++ <-> GLSL layout lock for the two structs the fragment reads every pixel
// (TerrainMaterialRecord/TerrainMaterialRecordData and CBTSurfaceParams/CBTSurfaceParamsData). No
// static_assert bridges C++ <-> GLSL, and a presence check cannot see a reorder, an insert or a
// scalar -> vecN change — all of which keep every name present while shifting every offset and
// corrupting every GPU read. So: parse the GLSL fields in declaration order, compute their std430
// offsets, and require name + type + offset + total size to match the C++ twin exactly.
TEST(CBTLayout, GlslSurfaceStructsMatchCppStd430)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(RENDERING_SHADER_INCLUDES_DIR)
    const std::string surfaceSrc =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_surface.glsl"));
    ASSERT_FALSE(surfaceSrc.empty()) << "cannot read cbt_surface.glsl";
    // TerrainMaterialRecordData is declared in the shared albedo include, not in either surface —
    // both of them reach it by including that file, so that file is where its layout is locked.
    const std::string albedoIncludeSrc = StripLineComments(
        SlurpFile(std::string(RENDERING_SHADER_INCLUDES_DIR) + "/terrain_material_albedo.glsl"));
    ASSERT_FALSE(albedoIncludeSrc.empty()) << "cannot read terrain_material_albedo.glsl";
    const std::string analyticSrc =
        StripLineComments(SlurpFile(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_analytic.glsl"));
    ASSERT_FALSE(analyticSrc.empty()) << "cannot read cbt_analytic.glsl";

    // CBT_MAX_SPHERE_ANALYTIC sizes the analytic tail on both sides.
    std::smatch cm;
    ASSERT_TRUE(std::regex_search(
        analyticSrc, cm, std::regex(R"(const\s+uint\s+CBT_MAX_SPHERE_ANALYTIC\s*=\s*([0-9]+)u?\s*;)")))
        << "CBT_MAX_SPHERE_ANALYTIC not found in cbt_analytic.glsl";
    const uint32_t glslMaxAnalytic = static_cast<uint32_t>(std::stoul(cm[1].str()));
    EXPECT_EQ(glslMaxAnalytic, kMaxSphereAnalyticModifiers)
        << "cbt_analytic.glsl CBT_MAX_SPHERE_ANALYTIC drifted from kMaxSphereAnalyticModifiers";
    const std::map<std::string, uint32_t> constants{{"CBT_MAX_SPHERE_ANALYTIC", glslMaxAnalytic}};

    std::map<std::string, Std430Layout> known;
    std::string err;

    // CBTSphereAnalyticFlatten — the element type of the surface params' analytic tail. It has no
    // named C++ twin (the CPU side stores the same bytes as a flat float array), so lock its size
    // against kSphereAnalyticFloatsPerModifier.
    std::vector<GlslField> analyticFields;
    ASSERT_TRUE(ParseGlslStructFields(analyticSrc, "CBTSphereAnalyticFlatten", constants,
                                      analyticFields, err))
        << err;
    Std430Layout analyticLayout;
    ASSERT_TRUE(ComputeStd430(analyticFields, known, analyticLayout, err)) << err;
    known["CBTSphereAnalyticFlatten"] = analyticLayout;
    EXPECT_EQ(static_cast<size_t>(analyticLayout.Size),
              kSphereAnalyticFloatsPerModifier * sizeof(float))
        << "cbt_analytic.glsl CBTSphereAnalyticFlatten no longer matches "
           "kSphereAnalyticFloatsPerModifier floats";

    auto checkFields = [](const char* glslName, const std::vector<GlslField>& parsed,
                          const Std430Layout& layout, const ExpectedField* expected, size_t count,
                          size_t cppSize) {
        ASSERT_EQ(parsed.size(), count)
            << glslName << " field COUNT drifted from its C++ twin (a field was added or removed)";
        for (size_t i = 0; i < count; ++i)
        {
            EXPECT_EQ(parsed[i].Name, std::string(expected[i].Name))
                << glslName << " field " << i << " is '" << parsed[i].Name << "', expected '"
                << expected[i].Name << "' — declaration ORDER drifted from the C++ twin";
            EXPECT_EQ(parsed[i].Type, std::string(expected[i].GlslType))
                << glslName << "." << expected[i].Name << " is declared '" << parsed[i].Type
                << "', expected '" << expected[i].GlslType << "'";
            EXPECT_EQ(parsed[i].ArrayLen, expected[i].ArrayLen)
                << glslName << "." << expected[i].Name << " array length drifted";
            EXPECT_EQ(static_cast<size_t>(layout.Offsets[i]), expected[i].Offset)
                << glslName << "." << expected[i].Name << " std430 offset is "
                << layout.Offsets[i] << ", C++ offsetof says " << expected[i].Offset;
        }
        EXPECT_EQ(static_cast<size_t>(layout.Size), cppSize)
            << glslName << " std430 size is " << layout.Size << ", sizeof(C++ twin) is " << cppSize;
    };

    // --- TerrainMaterialRecordData vs TerrainMaterialRecord. The material TABLE the fragment
    // indexes by slot ID. Same lock as above and for the same reason: the two padding floats are
    // exactly the fields a "harmless" cleanup deletes, and doing so shifts every UV phase read.
    const ExpectedField kRecordFields[] = {
        {"AlbedoR", "float", 1, offsetof(TerrainMaterialRecord, AlbedoR)},
        {"AlbedoG", "float", 1, offsetof(TerrainMaterialRecord, AlbedoG)},
        {"AlbedoB", "float", 1, offsetof(TerrainMaterialRecord, AlbedoB)},
        {"Tiling", "float", 1, offsetof(TerrainMaterialRecord, Tiling)},
        {"AlbedoTex", "uint", 1, offsetof(TerrainMaterialRecord, AlbedoTex)},
        {"NormalTex", "uint", 1, offsetof(TerrainMaterialRecord, NormalTex)},
        {"OrmTex", "uint", 1, offsetof(TerrainMaterialRecord, OrmTex)},
        {"Flags", "uint", 1, offsetof(TerrainMaterialRecord, Flags)},
        {"Roughness", "float", 1, offsetof(TerrainMaterialRecord, Roughness)},
        {"Ao", "float", 1, offsetof(TerrainMaterialRecord, Ao)},
        {"NormalStrength", "float", 1, offsetof(TerrainMaterialRecord, NormalStrength)},
        {"HexRotStrength", "float", 1, offsetof(TerrainMaterialRecord, HexRotStrength)},
        {"VariationStrength", "float", 1, offsetof(TerrainMaterialRecord, VariationStrength)},
        {"VariationHue", "float", 1, offsetof(TerrainMaterialRecord, VariationHue)},
        {"VariationScale", "float", 1, offsetof(TerrainMaterialRecord, VariationScale)},
        {"PlanarUVScaleX", "float", 1, offsetof(TerrainMaterialRecord, PlanarUVScaleX)},
        {"UVPhaseX", "float", 1, offsetof(TerrainMaterialRecord, UVPhaseX)},
        {"UVPhaseY", "float", 1, offsetof(TerrainMaterialRecord, UVPhaseY)},
        {"UVPhaseZ", "float", 1, offsetof(TerrainMaterialRecord, UVPhaseZ)},
        {"PlanarUVScaleZ", "float", 1, offsetof(TerrainMaterialRecord, PlanarUVScaleZ)},
    };
    std::vector<GlslField> recordFields;
    ASSERT_TRUE(ParseGlslStructFields(albedoIncludeSrc, "TerrainMaterialRecordData", constants,
                                      recordFields, err))
        << err;
    Std430Layout recordLayout;
    ASSERT_TRUE(ComputeStd430(recordFields, known, recordLayout, err)) << err;
    known["TerrainMaterialRecordData"] = recordLayout;

    checkFields("TerrainMaterialRecordData", recordFields, recordLayout, kRecordFields,
                std::size(kRecordFields), sizeof(TerrainMaterialRecord));

    // --- CBTSurfaceParamsData vs CBTSurfaceParams.
    const ExpectedField kSurfaceFields[] = {
        {"Radius", "float", 1, offsetof(CBTSurfaceParams, Radius)},
        {"ReliefAmplitude", "float", 1, offsetof(CBTSurfaceParams, ReliefAmplitude)},
        {"ReliefFrequency", "float", 1, offsetof(CBTSurfaceParams, ReliefFrequency)},
        {"ReliefOctaves", "uint", 1, offsetof(CBTSurfaceParams, ReliefOctaves)},
        {"SphereSculptEnabled", "uint", 1, offsetof(CBTSurfaceParams, SphereSculptEnabled)},
        {"SphereSculptVirtualDim", "uint", 1, offsetof(CBTSurfaceParams, SphereSculptVirtualDim)},
        {"SphereSculptCap", "uint", 1, offsetof(CBTSurfaceParams, SphereSculptCap)},
        {"SphereSculptPagesPerAxis", "uint", 1, offsetof(CBTSurfaceParams, SphereSculptPagesPerAxis)},
        {"SphereSculptPoolPageCount", "uint", 1, offsetof(CBTSurfaceParams, SphereSculptPoolPageCount)},
        {"DebugMode", "uint", 1, offsetof(CBTSurfaceParams, DebugMode)},
        {"AtlasBacked", "uint", 1, offsetof(CBTSurfaceParams, AtlasBacked)},
        {"AtlasDim", "uint", 1, offsetof(CBTSurfaceParams, AtlasDim)},
        {"AtlasSlotStride", "uint", 1, offsetof(CBTSurfaceParams, AtlasSlotStride)},
        {"AtlasSlotsPerRow", "uint", 1, offsetof(CBTSurfaceParams, AtlasSlotsPerRow)},
        {"AtlasTileRes", "uint", 1, offsetof(CBTSurfaceParams, AtlasTileRes)},
        {"AtlasTilesPerAxisX", "uint", 1, offsetof(CBTSurfaceParams, AtlasTilesPerAxisX)},
        {"AtlasTilesPerAxisZ", "uint", 1, offsetof(CBTSurfaceParams, AtlasTilesPerAxisZ)},
        {"AtlasCoarseDim", "uint", 1, offsetof(CBTSurfaceParams, AtlasCoarseDim)},
        {"AtlasSplatBindless", "uint", 1, offsetof(CBTSurfaceParams, AtlasSplatBindless)},
        {"AtlasNormalBindless", "uint", 1, offsetof(CBTSurfaceParams, AtlasNormalBindless)},
        {"AtlasSplatCoarseBindless", "uint", 1, offsetof(CBTSurfaceParams, AtlasSplatCoarseBindless)},
        {"AtlasNormalCoarseBindless", "uint", 1, offsetof(CBTSurfaceParams, AtlasNormalCoarseBindless)},
        {"SphereAnalyticCount", "uint", 1, offsetof(CBTSurfaceParams, SphereAnalyticCount)},
        {"SphereAnalyticDabCount", "uint", 1, offsetof(CBTSurfaceParams, SphereAnalyticDabCount)},
        {"SphereAnalytic", "CBTSphereAnalyticFlatten", kMaxSphereAnalyticModifiers,
         offsetof(CBTSurfaceParams, SphereAnalytic)},
    };
    std::vector<GlslField> surfaceFields;
    ASSERT_TRUE(
        ParseGlslStructFields(surfaceSrc, "CBTSurfaceParamsData", constants, surfaceFields, err))
        << err;
    Std430Layout surfaceLayout;
    ASSERT_TRUE(ComputeStd430(surfaceFields, known, surfaceLayout, err)) << err;

    checkFields("CBTSurfaceParamsData", surfaceFields, surfaceLayout, kSurfaceFields,
                std::size(kSurfaceFields), sizeof(CBTSurfaceParams));
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// TerrainParamsEntry is declared FOUR times in GLSL — cbt_surface.glsl, terrain_grass_surface.glsl,
// terrain_grass_vertex_modifier.glsl and terrain_grass_place.comp — all reading the SAME std430
// buffer (set 2 binding 1 in the three graphics stages, set 0 binding 0 in the compute stage, which
// binds its own set layout) and all mirroring one C++ struct (TerrainGPUParams). Nothing links them:
// four separate files spanning two modules, no static_assert, no shared include. Editing one and
// missing the others silently shifts every field past the edit for whichever shaders were missed, so
// the grass would read a terrain's roughness as its blade height and nothing would fail to compile.
//
// Locked pairwise rather than by four expectation tables: the invariant is that the mirrors agree
// with each other AND with the C++ twin's size, which is exactly what an edit to one breaks.
// TerrainParamsEntryMirrorSetIsComplete below is what keeps this list from going stale.
TEST(CBTLayout, TerrainParamsEntryMirrorsAgreeAcrossEveryShader)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(TERRAIN_GRASS_SHADER_SOURCE_DIR) && \
    defined(RENDERING_SHADER_INCLUDES_DIR)
    const std::vector<MirrorSource> sources = TerrainParamsMirrorSources();

    const std::map<std::string, uint32_t> constants;
    std::vector<std::vector<GlslField>> parsed(sources.size());
    std::vector<Std430Layout> layouts(sources.size());

    for (size_t i = 0; i < sources.size(); ++i)
    {
        const std::string src = StripLineComments(SlurpFile(sources[i].Path));
        ASSERT_FALSE(src.empty()) << "cannot read " << sources[i].Path;
        std::string err;
        ASSERT_TRUE(ParseGlslStructFields(src, "TerrainParamsEntry", constants, parsed[i], err))
            << sources[i].Label << ": " << err;
        std::map<std::string, Std430Layout> known;
        ASSERT_TRUE(ComputeStd430(parsed[i], known, layouts[i], err))
            << sources[i].Label << ": " << err;
        ASSERT_FALSE(parsed[i].empty()) << sources[i].Label << " parsed no fields";
    }

    // The C++ twin anchors the whole set — otherwise every mirror could agree on being wrong.
    EXPECT_EQ(static_cast<size_t>(layouts[0].Size), sizeof(GameEngine::Terrain::TerrainGPUParams))
        << "TerrainParamsEntry std430 size drifted from sizeof(TerrainGPUParams)";

    // A handful of offsets pinned against offsetof, chosen as the fields the material work moves.
    // A pure pairwise check cannot see every mirror drifting together from C++.
    const ExpectedField kAnchors[] = {
        {"SplatmapBindless", "uint", 1, offsetof(GameEngine::Terrain::TerrainGPUParams, SplatmapBindless)},
        {"MaterialTiling", "float", 1, offsetof(GameEngine::Terrain::TerrainGPUParams, MaterialTiling)},
        {"LayerCount", "uint", 1, offsetof(GameEngine::Terrain::TerrainGPUParams, LayerCount)},
        {"LayerRole", "uint", 4, offsetof(GameEngine::Terrain::TerrainGPUParams, LayerRole)},
        {"GrassLayerIndex", "uint", 1, offsetof(GameEngine::Terrain::TerrainGPUParams, GrassLayerIndex)},
        {"GrassControlBindless", "uint", 1, offsetof(GameEngine::Terrain::TerrainGPUParams, GrassControlBindless)},
    };
    for (const auto& anchor : kAnchors)
    {
        bool found = false;
        for (size_t f = 0; f < parsed[0].size(); ++f)
        {
            if (parsed[0][f].Name != anchor.Name)
                continue;
            found = true;
            EXPECT_EQ(parsed[0][f].Type, std::string(anchor.GlslType)) << anchor.Name;
            EXPECT_EQ(parsed[0][f].ArrayLen, anchor.ArrayLen) << anchor.Name;
            EXPECT_EQ(static_cast<size_t>(layouts[0].Offsets[f]), anchor.Offset)
                << "TerrainParamsEntry." << anchor.Name << " std430 offset drifted from the C++ twin";
            break;
        }
        EXPECT_TRUE(found) << "TerrainParamsEntry." << anchor.Name << " is gone from cbt_surface.glsl";
    }

    // Now the duplication itself: every mirror must match mirror 0 field for field.
    for (size_t i = 1; i < sources.size(); ++i)
    {
        ASSERT_EQ(parsed[i].size(), parsed[0].size())
            << sources[i].Label << " declares " << parsed[i].size()
            << " TerrainParamsEntry fields, " << sources[0].Label << " declares " << parsed[0].size()
            << " — a field was added or removed in one mirror only";
        for (size_t f = 0; f < parsed[0].size(); ++f)
        {
            EXPECT_EQ(parsed[i][f].Name, parsed[0][f].Name)
                << sources[i].Label << " field " << f << " is '" << parsed[i][f].Name
                << "', " << sources[0].Label << " has '" << parsed[0][f].Name
                << "' — declaration ORDER drifted between mirrors";
            EXPECT_EQ(parsed[i][f].Type, parsed[0][f].Type)
                << sources[i].Label << "." << parsed[0][f].Name << " type drifted";
            EXPECT_EQ(parsed[i][f].ArrayLen, parsed[0][f].ArrayLen)
                << sources[i].Label << "." << parsed[0][f].Name << " array length drifted";
            EXPECT_EQ(layouts[i].Offsets[f], layouts[0].Offsets[f])
                << sources[i].Label << "." << parsed[0][f].Name << " std430 offset drifted";
        }
        EXPECT_EQ(layouts[i].Size, layouts[0].Size)
            << sources[i].Label << " TerrainParamsEntry total size drifted";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR / TERRAIN_GRASS_SHADER_SOURCE_DIR not defined";
#endif
}

// The mirror list above is a hand-written enumeration, and an enumeration of a duplicated
// declaration is exactly the thing that goes quietly stale — the list shipped covering three of the
// four declarations that existed, so the compute mirror was unguarded while the test read as a
// complete lock. This rediscovers the set by scanning every shader under Engine/Modules and requires
// it to equal the guarded set, so a fifth mirror reds this test on the commit that adds it rather
// than corrupting grass placement on the commit that edits the struct.
TEST(CBTLayout, TerrainParamsEntryMirrorSetIsComplete)
{
#if defined(GE_ENGINE_MODULES_DIR) && defined(CBT_SHADER_SOURCE_DIR) && \
    defined(TERRAIN_GRASS_SHADER_SOURCE_DIR) && \
    defined(RENDERING_SHADER_INCLUDES_DIR)
    ASSERT_TRUE(std::filesystem::is_directory(std::filesystem::path{GE_ENGINE_MODULES_DIR}))
        << "cannot scan " << GE_ENGINE_MODULES_DIR;

    EXPECT_EQ(DiscoverGlslStructDeclarations("TerrainParamsEntry"),
              MirrorLabels(TerrainParamsMirrorSources()))
        << "the set of shaders declaring TerrainParamsEntry no longer matches the set the drift lock "
           "reads. Add the new file to TerrainParamsMirrorSources() (or delete the stale entry) — an "
           "unguarded mirror silently shifts every field past the next edit.";
#else
    GTEST_SKIP() << "GE_ENGINE_MODULES_DIR / shader source dirs not defined";
#endif
}

// TerrainMaterialRecordData is declared in BOTH terrain surfaces, reading one shared table. A
// drift here does not shift a neighbouring field the way TerrainParamsEntry does — it makes the
// grass read a different material's fields than the ground under it, which looks like a lighting
// or tinting bug rather than a layout bug.
TEST(CBTLayout, TerrainMaterialRecordMirrorsAgreeAcrossEveryShader)
{
#if defined(CBT_SHADER_SOURCE_DIR) && defined(TERRAIN_GRASS_SHADER_SOURCE_DIR) && \
    defined(RENDERING_SHADER_INCLUDES_DIR)
    const std::vector<MirrorSource> sources = TerrainMaterialRecordMirrorSources();

    const std::map<std::string, uint32_t> constants;
    std::vector<std::vector<GlslField>> parsed(sources.size());
    std::vector<Std430Layout> layouts(sources.size());

    for (size_t i = 0; i < sources.size(); ++i)
    {
        const std::string src = StripLineComments(SlurpFile(sources[i].Path));
        ASSERT_FALSE(src.empty()) << "cannot read " << sources[i].Path;
        std::string err;
        ASSERT_TRUE(ParseGlslStructFields(src, "TerrainMaterialRecordData", constants, parsed[i], err))
            << sources[i].Label << ": " << err;
        std::map<std::string, Std430Layout> known;
        ASSERT_TRUE(ComputeStd430(parsed[i], known, layouts[i], err)) << sources[i].Label << ": " << err;
        ASSERT_FALSE(parsed[i].empty()) << sources[i].Label << " parsed no fields";
    }

    // The C++ twin anchors the set — otherwise every mirror could agree on being wrong. Field-level
    // agreement with C++ is GlslSurfaceStructsMatchCppStd430's job; this pins the size and the two
    // fields the read sites index by name.
    EXPECT_EQ(static_cast<size_t>(layouts[0].Size), sizeof(TerrainMaterialRecord))
        << "TerrainMaterialRecordData std430 size drifted from sizeof(TerrainMaterialRecord)";
    const ExpectedField kAnchors[] = {
        {"AlbedoTex", "uint", 1, offsetof(TerrainMaterialRecord, AlbedoTex)},
        {"Roughness", "float", 1, offsetof(TerrainMaterialRecord, Roughness)},
    };
    for (const auto& anchor : kAnchors)
    {
        bool found = false;
        for (size_t f = 0; f < parsed[0].size(); ++f)
        {
            if (parsed[0][f].Name != anchor.Name)
                continue;
            found = true;
            EXPECT_EQ(parsed[0][f].Type, std::string(anchor.GlslType)) << anchor.Name;
            EXPECT_EQ(static_cast<size_t>(layouts[0].Offsets[f]), anchor.Offset)
                << "TerrainMaterialRecordData." << anchor.Name << " std430 offset drifted from C++";
            break;
        }
        EXPECT_TRUE(found) << "TerrainMaterialRecordData." << anchor.Name
                           << " is gone from cbt_surface.glsl";
    }

    for (size_t i = 1; i < sources.size(); ++i)
    {
        ASSERT_EQ(parsed[i].size(), parsed[0].size())
            << sources[i].Label << " declares " << parsed[i].size()
            << " TerrainMaterialRecordData fields, " << sources[0].Label << " declares "
            << parsed[0].size() << " — a field was added or removed in one mirror only";
        for (size_t f = 0; f < parsed[0].size(); ++f)
        {
            EXPECT_EQ(parsed[i][f].Name, parsed[0][f].Name)
                << sources[i].Label << " field " << f << " is '" << parsed[i][f].Name << "', "
                << sources[0].Label << " has '" << parsed[0][f].Name
                << "' — declaration ORDER drifted between mirrors";
            EXPECT_EQ(parsed[i][f].Type, parsed[0][f].Type)
                << sources[i].Label << "." << parsed[0][f].Name << " type drifted";
            EXPECT_EQ(parsed[i][f].ArrayLen, parsed[0][f].ArrayLen)
                << sources[i].Label << "." << parsed[0][f].Name << " array length drifted";
            EXPECT_EQ(layouts[i].Offsets[f], layouts[0].Offsets[f])
                << sources[i].Label << "." << parsed[0][f].Name << " std430 offset drifted";
        }
        EXPECT_EQ(layouts[i].Size, layouts[0].Size)
            << sources[i].Label << " TerrainMaterialRecordData total size drifted";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR / TERRAIN_GRASS_SHADER_SOURCE_DIR not defined";
#endif
}

// The record's list needs its OWN rediscovery, not the params one: the two sets differ (the vertex
// modifier and the placement compute mirror the params but not the record), so a sweep for one
// says nothing about the other. Without this, a third surface mirroring the record would ship
// unguarded — the exact way the compute mirror of TerrainParamsEntry was missed.
TEST(CBTLayout, TerrainMaterialRecordMirrorSetIsComplete)
{
#if defined(GE_ENGINE_MODULES_DIR) && defined(CBT_SHADER_SOURCE_DIR) && \
    defined(TERRAIN_GRASS_SHADER_SOURCE_DIR) && \
    defined(RENDERING_SHADER_INCLUDES_DIR)
    ASSERT_TRUE(std::filesystem::is_directory(std::filesystem::path{GE_ENGINE_MODULES_DIR}))
        << "cannot scan " << GE_ENGINE_MODULES_DIR;

    EXPECT_EQ(DiscoverGlslStructDeclarations("TerrainMaterialRecordData"),
              MirrorLabels(TerrainMaterialRecordMirrorSources()))
        << "the set of shaders declaring TerrainMaterialRecordData no longer matches the set the "
           "drift lock reads. Add the new file to TerrainMaterialRecordMirrorSources() (or delete "
           "the stale entry) — an unguarded mirror reads another material's fields.";
#else
    GTEST_SKIP() << "GE_ENGINE_MODULES_DIR / shader source dirs not defined";
#endif
}

TEST(CBTLayout, PoolAndBaseDepth)
{
    EXPECT_EQ(kDefaultBisectorPoolSize, 1048576u); // 2^20 (1M pool bump)
    EXPECT_EQ(kDefaultBaseDepth, 1u);             // log2(2 twin triangles)
    EXPECT_EQ(kRootHalfedgeCount, 2u);
    EXPECT_EQ(kComputeWorkgroupSize, 64u);
    EXPECT_EQ(kCBTBindingCount, 14u);            // SoA storage buffers
    EXPECT_EQ(kCBTFrameParamsBinding, 14u);      // C4 params UBO
    EXPECT_EQ(kCBTHeightTextureBinding, 15u);    // C4 height sampler
    EXPECT_EQ(kCBTSphereSculptBinding, 16u);     // planet editing: sphere sculpt page POOL SSBO
    EXPECT_EQ(kCBTAtlasRowsBinding, 17u);        // Phase E: atlas indirection SSBO
    EXPECT_EQ(kCBTAtlasHeightBinding, 18u);      // Phase E: atlas height texture ring
    EXPECT_EQ(kCBTAtlasCoarseBinding, 19u);      // Risk 3: out-of-window coarse height field ring
    EXPECT_EQ(kCBTSphereSculptPageTableBinding, 20u); // planet editing v2: sphere sculpt page TABLE SSBO
    EXPECT_EQ(kCBTPageTableBinding, 22u);        // paged height resolve: page table SSBO ring
    EXPECT_EQ(kCBTPageCacheBinding, 23u);        // paged height resolve: page cache texture ring
    EXPECT_EQ(kCBTDescriptorBindingCount, 24u);
}

// The height-range pyramid's three constants are hand-mirrored between CBTLayout.h and cbt_layout.glsl
// (the CPU sizes the buffer and the build dispatches, the kernels address it). Parse the GLSL defines
// and lock them to the C++ constants.
TEST(CBTLayout, GlslHeightRangeConstantsMatchCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    std::smatch m;
    const std::regex entries(R"(#define\s+CBT_HEIGHT_RANGE_ENTRIES\s+\(1u\s*<<\s*([0-9]+)\))");
    ASSERT_TRUE(std::regex_search(src, m, entries)) << "CBT_HEIGHT_RANGE_ENTRIES not found";
    EXPECT_EQ(1u << std::stoul(m[1].str()), kCBTHeightRangeEntries);
    const std::regex firstLevel(R"(#define\s+CBT_HEIGHT_RANGE_MAX_FIRST_LEVEL\s+([0-9]+)u)");
    ASSERT_TRUE(std::regex_search(src, m, firstLevel)) << "CBT_HEIGHT_RANGE_MAX_FIRST_LEVEL not found";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kCBTHeightRangeMaxFirstLevel);
    const std::regex header(R"(#define\s+CBT_HEIGHT_RANGE_HEADER\s+([0-9]+)u)");
    ASSERT_TRUE(std::regex_search(src, m, header)) << "CBT_HEIGHT_RANGE_HEADER not found";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kCBTHeightRangeHeaderWords);
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The paged height resolve's page-table layout is hand-mirrored between CBTLayout.h (TerrainECS packs
// the words, CBTResources sizes the ring) and cbt_layout.glsl (the taps read them). Lock the four.
TEST(CBTLayout, GlslPageTableConstantsMatchCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    std::smatch m;
    const std::regex header(R"(#define\s+CBT_PAGE_HEADER_WORDS\s+([0-9]+)u)");
    ASSERT_TRUE(std::regex_search(src, m, header)) << "CBT_PAGE_HEADER_WORDS not found";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kCBTPageHeaderWords);
    const std::regex levels(R"(#define\s+CBT_PAGE_MAX_LEVELS\s+([0-9]+)u)");
    ASSERT_TRUE(std::regex_search(src, m, levels)) << "CBT_PAGE_MAX_LEVELS not found";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kCBTPageMaxLevels);
    const std::regex fades(R"(#define\s+CBT_PAGE_FADE_OFFSET\s+\(CBT_PAGE_HEADER_WORDS\s*\+\s*([0-9]+)u\s*\*\s*CBT_PAGE_MAX_LEVELS\))");
    ASSERT_TRUE(std::regex_search(src, m, fades)) << "CBT_PAGE_FADE_OFFSET not found";
    EXPECT_EQ(kCBTPageHeaderWords + static_cast<uint32_t>(std::stoul(m[1].str())) * kCBTPageMaxLevels,
              kCBTPageFadeOffsetWords);
    const std::regex lane(R"(#define\s+CBT_PAGE_RING_WORDS_LANE\s+([0-9]+))");
    ASSERT_TRUE(std::regex_search(src, m, lane)) << "CBT_PAGE_RING_WORDS_LANE not found";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kCBTPageRingWordsLane);
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The atlas indirection cap (rows per ring slot) is hand-mirrored between the C++ kAtlasMaxTiles and
// the GLSL CBT_ATLAS_MAX_TILES (single-sourced in cbt_atlas.glsl, which both the compute layout and the
// surface include). No static_assert can bridge C++ <-> GLSL, so parse the GLSL literal and lock it to
// the C++ constant — a future edit to one without the other fails here.
TEST(CBTLayout, GlslAtlasMaxTilesMatchesCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_atlas.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    std::smatch m;
    const std::regex re(R"(const\s+uint\s+CBT_ATLAS_MAX_TILES\s*=\s*([0-9]+)u?\s*;)");
    ASSERT_TRUE(std::regex_search(src, m, re)) << "CBT_ATLAS_MAX_TILES not found in cbt_atlas.glsl";
    const uint32_t glslValue = static_cast<uint32_t>(std::stoul(m[1].str()));
    EXPECT_EQ(glslValue, kAtlasMaxTiles)
        << "cbt_atlas.glsl CBT_ATLAS_MAX_TILES (" << glslValue << ") != C++ kAtlasMaxTiles ("
        << kAtlasMaxTiles << ") — the atlas indirection cap drifted out of lockstep";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The reduce's split level is a C++ constant (its fit is asserted in CBTLayout.h) and a kernel
// constant; they must agree.
TEST(CBTLayout, GlslSumTreeSharedDepthMatchesCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string kernelPath = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_kernels.comp";
    std::ifstream file(kernelPath);
    ASSERT_TRUE(file.is_open()) << "cannot open " << kernelPath;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();
    std::smatch m;
    const std::regex re(R"(const\s+uint\s+CBT_SUMTREE_SHARED_DEPTH\s*=\s*([0-9]+)u?\s*;)");
    ASSERT_TRUE(std::regex_search(src, m, re)) << "CBT_SUMTREE_SHARED_DEPTH not found in cbt_kernels.comp";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kSumTreeSharedDepth);
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The decode-precision cap is single-sourced: kMaxDecodeSubdiv (CBTLayout.h) drives both host
// clamp sites (TerrainProvisioning) and mirrors the shader guard CBT_MAX_NUM_SUBDIV in
// cbt_layout.glsl (used by Kernel_VertexEval's `numSubdiv > CBT_MAX_NUM_SUBDIV` return). No
// static_assert can bridge C++ <-> GLSL, so parse the GLSL literal and lock it to the C++
// constant — a future edit to one without the other fails
// here, which would silently desync the host derivation from the on-GPU decode ceiling.
TEST(CBTLayout, GlslMaxDecodeSubdivMatchesCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string layoutPath = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl";
    std::ifstream file(layoutPath);
    ASSERT_TRUE(file.is_open()) << "cannot open " << layoutPath;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    std::smatch m;
    const std::regex re(R"(const\s+uint\s+CBT_MAX_NUM_SUBDIV\s*=\s*([0-9]+)u?\s*;)");
    ASSERT_TRUE(std::regex_search(src, m, re)) << "CBT_MAX_NUM_SUBDIV not found in cbt_layout.glsl";
    const uint32_t glslValue = static_cast<uint32_t>(std::stoul(m[1].str()));
    EXPECT_EQ(glslValue, kMaxDecodeSubdiv)
        << "cbt_layout.glsl CBT_MAX_NUM_SUBDIV (" << glslValue << ") != C++ kMaxDecodeSubdiv ("
        << kMaxDecodeSubdiv << ") — the decode depth cap drifted out of lockstep";

    // The kernel guard must reference the named constants, not bare literals, or a future cap
    // bump here would silently leave a stale hard-coded return in Kernel_VertexEval. S2a: the
    // guard became mode-selected — `decodeCap = deep ? CBT_DEEP_NUM_SUBDIV : CBT_MAX_NUM_SUBDIV`
    // (the DeepDecode::SubdivCap mirror) — so the lock moved from the old literal substring
    // `numSubdiv > CBT_MAX_NUM_SUBDIV` to the selection expression + the guarded compare.
    const std::string kernelPath = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_kernels.comp";
    std::ifstream kfile(kernelPath);
    ASSERT_TRUE(kfile.is_open()) << "cannot open " << kernelPath;
    std::stringstream kss;
    kss << kfile.rdbuf();
    EXPECT_NE(kss.str().find("CBT_DEEP_NUM_SUBDIV : CBT_MAX_NUM_SUBDIV_EFFECTIVE"),
              std::string::npos)
        << "Kernel_VertexEval must select its decode cap from the two named constants "
           "(DeepDecode::SubdivCap mirror), not a literal";
    EXPECT_NE(kss.str().find("numSubdiv > decodeCap"), std::string::npos)
        << "Kernel_VertexEval must gate on the selected decodeCap";

    // The narrow-heap arm's cap is the u32 heap's own ceiling, not a preference: a drift
    // between it and the host clamp lets Classify grow a bisector past the depth its heap
    // ID can represent, and the decode then reads a truncated ID. Same lock, same reason.
    const std::regex heap32Re(
        R"(const\s+uint\s+CBT_MAX_NUM_SUBDIV_EFFECTIVE\s*=\s*([0-9]+)u?\s*;)");
    ASSERT_TRUE(std::regex_search(src, m, heap32Re))
        << "CBT_MAX_NUM_SUBDIV_EFFECTIVE not found in cbt_layout.glsl";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kHeap32DecodeSubdiv)
        << "cbt_layout.glsl's narrow-arm CBT_MAX_NUM_SUBDIV_EFFECTIVE != C++ "
           "kHeap32DecodeSubdiv — the narrow decode cap drifted out of lockstep";
    // A u32 heap ID holds depth = baseDepth + numSubdiv below 32, and the walk's
    // scale = 1 << numSubdiv must stay inside a signed 32-bit integer.
    EXPECT_LT(kSphereBaseDepth + kHeap32DecodeSubdiv, 32u)
        << "kHeap32DecodeSubdiv exceeds what a u32 heap ID can carry at the deepest base mesh";
    EXPECT_LT(kHeap32DecodeSubdiv, 31u)
        << "kHeap32DecodeSubdiv overflows the narrow arm's signed barycentric scale";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The dispatch-clamp contract is hand-mirrored: kWQDispatchClampCounter (CBTLayout.h)
// <-> CBT_WQ_DISPATCH_CLAMP_COUNTER (cbt_layout.glsl), and both GPU-written dispatch-arg
// writers (Kernel_Reset, Kernel_PrepareIndirect) must route through the bounded helper.
// Parse the GLSL and lock it to the C++ constant so a
// future kernel edit cannot silently bypass the bound or desync the telemetry slot the
// readback tests assert on.
// The planar merge-metric suite hand-mirrors these shader constants to predict the GPU's
// split/merge decisions; a silent drift lands inside its rounding-tolerance band instead of
// producing loud disagreements, so the values are locked here at the source.
TEST(CBTLayout, GlslPlanarMetricConstantsMirrorCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    auto slurp = [](const std::string& path, std::string& out) {
        std::ifstream f(path);
        if (!f.is_open())
            return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    };

    std::string kernels;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_kernels.comp", kernels));
    std::string layout;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl", layout));

    auto lockFloat = [](const std::string& src, const char* name, float expected) {
        const std::regex re(std::string(R"(const\s+float\s+)") + name +
                            R"(\s*=\s*([0-9]*\.?[0-9]+)\s*;)");
        std::smatch m;
        ASSERT_TRUE(std::regex_search(src, m, re)) << name << " not found in shader source";
        EXPECT_FLOAT_EQ(std::stof(m[1].str()), expected)
            << name << " drifted out of lockstep with the merge-metric suite's CPU mirror";
    };

    lockFloat(kernels, "CBT_SPLIT_NDC_MARGIN", 1.1f);
    lockFloat(kernels, "CBT_KEEP_RAMP", 0.30f);
    lockFloat(kernels, "CBT_LEB_PARENT_EDGE_SCALE", 1.41421356f);
    lockFloat(layout, "CBT_NEAR_BIAS_CONTENTION_LO", 0.50f);
    lockFloat(layout, "CBT_NEAR_BIAS_CONTENTION_HI", 0.90f);
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

TEST(CBTLayout, GlslDispatchClampMirrorsCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    auto slurp = [](const std::string& path, std::string& out) {
        std::ifstream f(path);
        if (!f.is_open())
            return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    };

    std::string layout;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl", layout));
    std::smatch m;
    const std::regex slotRe(R"(const\s+uint\s+CBT_WQ_DISPATCH_CLAMP_COUNTER\s*=\s*([0-9]+)u?\s*;)");
    ASSERT_TRUE(std::regex_search(layout, m, slotRe))
        << "CBT_WQ_DISPATCH_CLAMP_COUNTER not found in cbt_layout.glsl";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), kWQDispatchClampCounter)
        << "the clamp telemetry slot drifted out of lockstep with kWQDispatchClampCounter";

    std::string kernels;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_kernels.comp", kernels));
    // The definition plus at least the two writer call sites (Kernel_Reset,
    // Kernel_PrepareIndirect) — a writer that stops routing through the helper
    // regains an unbounded dispatch width.
    size_t occurrences = 0;
    for (size_t pos = kernels.find("CBT_BoundedDispatchGroups("); pos != std::string::npos;
         pos = kernels.find("CBT_BoundedDispatchGroups(", pos + 1))
        ++occurrences;
    EXPECT_GE(occurrences, 3u)
        << "expected the CBT_BoundedDispatchGroups definition plus call sites in Kernel_Reset "
           "and Kernel_PrepareIndirect; a dispatch-arg writer bypasses the bound";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The two planar pool controllers Kernel_Reset steps are hand-mirrored. The pool-pressure controller:
// the work-queue slot it keeps its step in and the octave divisor (CBTLayout.h <-> cbt_layout.glsl),
// and its two occupancy marks, which are not new numbers but the engine's existing saturation and
// pressure constants (kCBTSaturatedOccupancy, kOffFrustumKeepOcc). The off-frustum keep step: its
// slot (CBTLayout.h) and its band schedule (CBTDemandTuning.h). Each drifts silently in its own way:
// a slot drift makes the host readback and Kernel_Classify read different words (or the two
// controllers share one), an octave drift makes get_terrain_stats report a pressureScale the kernel
// never applied, a mark drift moves the controller's band away from the band the rest of the demand
// shaping assumes, and a schedule drift makes the band the kernel applies differ from the one the
// tests reason about. Parse the GLSL and lock all of them to the C++ (the GlslDispatchClampMirrorsCpp
// pattern).
TEST(CBTLayout, GlslPressureConstantsMirrorCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    std::ifstream file(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl");
    ASSERT_TRUE(file.is_open()) << "cannot open cbt_layout.glsl";
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string layout = ss.str();

    auto lockUint = [&layout](const char* name, uint32_t expected) {
        const std::regex re(std::string(R"(const\s+uint\s+)") + name + R"(\s*=\s*([0-9]+)u?\s*;)");
        std::smatch m;
        ASSERT_TRUE(std::regex_search(layout, m, re)) << name << " not found in cbt_layout.glsl";
        EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), expected)
            << name << " drifted out of lockstep with its CBTLayout.h mirror";
    };
    auto lockFloat = [&layout](const char* name, double expected) {
        const std::regex re(std::string(R"(const\s+float\s+)") + name +
                            R"(\s*=\s*([0-9]*\.?[0-9]+)\s*;)");
        std::smatch m;
        ASSERT_TRUE(std::regex_search(layout, m, re)) << name << " not found in cbt_layout.glsl";
        EXPECT_FLOAT_EQ(std::stof(m[1].str()), static_cast<float>(expected))
            << name << " drifted off the C++ constant it mirrors";
    };

    lockUint("CBT_WQ_PRESSURE_STEP", kWQPressureStep);
    lockUint("CBT_PRESSURE_STEPS_PER_OCTAVE", kPressureStepsPerOctave);
    lockFloat("CBT_PRESSURE_FULL_OCC", kCBTSaturatedOccupancy);
    lockFloat("CBT_PRESSURE_RECOVER_OCC", static_cast<double>(kOffFrustumKeepOcc));

    lockUint("CBT_WQ_OFF_FRUSTUM_KEEP_STEP", kWQOffFrustumKeepStep);
    lockUint("CBT_OFF_FRUSTUM_KEEP_STEPS", kOffFrustumKeepSteps);
    lockUint("CBT_OFF_FRUSTUM_KEEP_STEPS_PER_OCTAVE", kOffFrustumKeepStepsPerOctave);
    lockUint("CBT_OFF_FRUSTUM_KEEP_SATURATED_STRIDE", kOffFrustumKeepSaturatedStride);
    lockFloat("CBT_OFF_FRUSTUM_KEEP_WIDEST_NDC", static_cast<double>(kOffFrustumKeepWidestNdc));

    // Both steps live in the counter block the host reads back in one copy; a slot beyond it would
    // leave the stats reading whatever the payload lanes hold.
    EXPECT_LT(kWQPressureStep, kWQCounterSlots)
        << "the pressure step slot sits outside the counter block the readback copies";
    EXPECT_LT(kWQOffFrustumKeepStep, kWQCounterSlots)
        << "the keep step slot sits outside the counter block the readback copies";
    EXPECT_NE(kWQOffFrustumKeepStep, kWQPressureStep) << "the two controllers would share one word";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The validation counter slots are hand-mirrored: kValidation*Counter (CBTLayout.h) <->
// CBT_VALIDATION_*_COUNTER (cbt_layout.glsl). The host reads a counter BY SLOT INDEX out of the
// readback buffer, so a slot that drifts between the two makes every validation assertion read the
// wrong word — green tests over a broken invariant. Parse the GLSL and lock it to the C++ (the
// GlslDispatchClampMirrorsCpp pattern), and pin that Kernel_Reset zeroes every slot: an unzeroed
// counter accumulates across frames, so the first transient failure would never clear.
TEST(CBTLayout, GlslValidationCountersMatchCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    auto slurp = [](const std::string& path, std::string& out) {
        std::ifstream f(path);
        if (!f.is_open())
            return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    };

    std::string layout;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl", layout));
    std::string kernels;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_kernels.comp", kernels));

    const std::pair<const char*, uint32_t> slots[] = {
        {"CBT_VALIDATION_ERROR_COUNTER", kValidationErrorCounter},
        {"CBT_VALIDATION_BUDGET_COUNTER", kValidationBudgetCounter},
        {"CBT_VALIDATION_ZOMBIE_COUNTER", kValidationZombieCounter},
        {"CBT_VALIDATION_COMPACT_COUNTER", kValidationCompactCounter},
    };
    for (const auto& [name, cppValue] : slots)
    {
        std::smatch m;
        const std::regex re(std::string("const\\s+uint\\s+") + name + R"(\s*=\s*([0-9]+)u?\s*;)");
        ASSERT_TRUE(std::regex_search(layout, m, re)) << name << " not found in cbt_layout.glsl";
        EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), cppValue)
            << name << " drifted out of lockstep with its C++ mirror — the host reads validation "
                       "counters by slot index";
        EXPECT_NE(kernels.find(std::string("Validation[") + name + "] = 0u"), std::string::npos)
            << "Kernel_Reset must zero " << name << " every frame, or it accumulates across frames";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// S2a lockstep: the deep (sector, local) GLSL library must mirror the CPU authority.
//   * CBT_DEEP_NUM_SUBDIV (cbt_deep_decode.glsl) == DeepDecode::kDeepDecodeSubdiv.
//   * The GLSL CBTVertexData carries the 4 x uvec2 sector tail (struct parse, the
//     MaterialLayerPaletteContract pattern).
//   * The sector lane packers roundtrip (CPU twin), including negative components, and the
//     16-bit lane range covers Earth with >= 5x margin (the honest packing bound: the S2a
//     layout is 96B BECAUSE the sectors are 16-bit lanes — a full ivec3 tail would be 112B).
TEST(CBTLayout, GlslDeepDecodeMirrorsCpp)
{
    // Packing roundtrip incl. negatives and the range corners.
    for (const int32_t s : {0, 1, -1, 511, -512, 6223, -6223, 16384, -16384, 32767, -32768})
    {
        EXPECT_EQ(UnpackSectorLo(PackSectorPair(s, 12345)), s);
        EXPECT_EQ(UnpackSectorHi(PackSectorPair(-321, s)), s);
    }
    // Earth (R = 6.371e6 m) sector magnitude ~6223 — the 16-bit lane range (+/-32767) holds
    // >= 5x margin; the fp32 sector*1024 exactness bound (2^24 m -> +/-16384 sectors, the
    // engine-wide render-origin bound) is the tighter honest limit and still covers 2.6x Earth.
    EXPECT_GE(32767, 5 * 6223);
    EXPECT_GE(16384, 2 * 6223);

#if defined(CBT_SHADER_SOURCE_DIR)
    auto slurp = [](const std::string& path, std::string& out) {
        std::ifstream f(path);
        if (!f.is_open())
            return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    };
    std::string deep;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_deep_decode.glsl", deep));
    std::smatch m;
    const std::regex capRe(R"(const\s+uint\s+CBT_DEEP_NUM_SUBDIV\s*=\s*([0-9]+)u?\s*;)");
    ASSERT_TRUE(std::regex_search(deep, m, capRe)) << "CBT_DEEP_NUM_SUBDIV not found";
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())), DeepDecode::kDeepDecodeSubdiv)
        << "cbt_deep_decode.glsl CBT_DEEP_NUM_SUBDIV drifted from DeepDecode::kDeepDecodeSubdiv";
    // The bit-parity contract's two shared conventions must stay present verbatim.
    EXPECT_NE(deep.find("floor(w.Hi * CBT_DEEP_INV_SECTOR + 0.5)"), std::string::npos)
        << "the shared floor(x + 0.5) sector tie convention left the GLSL twin";
    EXPECT_NE(deep.find("0x5F3759DFu"), std::string::npos)
        << "the shared deterministic rsqrt seed left the GLSL twin";

    std::string layout;
    ASSERT_TRUE(slurp(std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl", layout));
    for (const char* field : {"uvec2 sector0;", "uvec2 sector1;", "uvec2 sector2;", "uvec2 deepTag;"})
        EXPECT_NE(layout.find(field), std::string::npos)
            << "cbt_layout.glsl CBTVertexData lost the S2a sector tail field: " << field;
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// The sculpt page-table constants are hand-mirrored between C++ (CBTLayout.h kSculpt*) and GLSL
// (cbt_sculpt.glsl CBT_SCULPT_*). Parse the GLSL literals and lock them to the C++ constants so a
// future edit to one without the other fails here (the CPU/GPU page-resolve bit-lock).
TEST(CBTLayout, GlslSculptPageConstantsMatchCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_sculpt.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    auto glslConst = [&](const char* name) -> uint32_t {
        std::smatch m;
        const std::regex re(std::string("const\\s+uint\\s+") + name + "\\s*=\\s*([0-9]+)u?\\s*;");
        EXPECT_TRUE(std::regex_search(src, m, re)) << name << " not found in cbt_sculpt.glsl";
        return m.empty() ? 0u : static_cast<uint32_t>(std::stoul(m[1].str()));
    };

    EXPECT_EQ(glslConst("CBT_SCULPT_PAGE_DIM"), kSculptPageDim);
    EXPECT_EQ(glslConst("CBT_SCULPT_MAX_PAGES_PER_FACE_AXIS"), kSculptMaxPagesPerFaceAxis);
    // S4 adaptive page levels: the entry decode + level cap must stay in lockstep — the GPU
    // sampler addresses escalated blocks from these three constants alone.
    EXPECT_EQ(glslConst("CBT_SCULPT_PAGE_LEVEL_SHIFT"), kSculptPageLevelShift);
    EXPECT_EQ(glslConst("CBT_SCULPT_MAX_PAGE_LEVEL"), kSculptMaxPageLevel);
    // Hex literals matched separately.
    std::smatch mn;
    ASSERT_TRUE(std::regex_search(
        src, mn, std::regex(R"(const\s+uint\s+CBT_SCULPT_NO_PAGE\s*=\s*(0x[0-9A-Fa-f]+)u?\s*;)")));
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(mn[1].str(), nullptr, 16)), kSculptNoPage);
    ASSERT_TRUE(std::regex_search(
        src, mn,
        std::regex(R"(const\s+uint\s+CBT_SCULPT_PAGE_ID_MASK\s*=\s*(0x[0-9A-Fa-f]+)u?\s*;)")));
    EXPECT_EQ(static_cast<uint32_t>(std::stoul(mn[1].str(), nullptr, 16)), kSculptPageIdMask);
    // The fine-grid dim formula must exist verbatim on the GPU side (the (127*2^L + 1)^2
    // endpoint-aligned block layout both sides address).
    EXPECT_NE(src.find("127u * (1u << level) + 1u"), std::string::npos)
        << "cbt_sculpt.glsl CBT_SculptFineDim lost the endpoint-aligned fine-grid formula";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

TEST(CBTLayout, U32SumTreeDerivation)
{
    // u32 leaf-word re-derivation (plan §6). Reference used P/64 (u64 words); we
    // use P/32, so leaf words and leaf nodes double.
    EXPECT_EQ(CBTBitfieldWords(kDefaultBisectorPoolSize), 32768u);
    EXPECT_EQ(CBTLeafNodeCount(kDefaultBisectorPoolSize), 32768u);
    EXPECT_EQ(CBTLeafPackedWords(kDefaultBisectorPoolSize), 8192u); // 4 packed byte-counts/u32
    EXPECT_EQ(CBTSumTreeNodeCount(kDefaultBisectorPoolSize), 65535u); // 2*32768 - 1
    EXPECT_EQ(CBTLeafLevelOffset(kDefaultBisectorPoolSize), 32767u);
    EXPECT_EQ(CBTSumTreeDepth(kDefaultBisectorPoolSize), 15u); // log2(32768)
}

// Slot-diet round 2: the six logical work queues share THREE aliased physical payload lanes
// (their per-frame live spans are pairwise disjoint within each lane — the interval-graph
// 3-colouring proven at the CBTLayout.h definition). Lock the lane assignment: queues that share
// a lane resolve to the SAME offset, the three lanes are mutually distinct, and the buffer is
// 16 + 3*P words (was 16 + 6*P — the −12 B/slot diet). A regression that un-aliases a lane or
// re-overlaps two live spans changes these offsets and fails here.
TEST(CBTLayout, WorkQueuePayloadLanesAliasThreeLanes)
{
    const uint32_t p = kDefaultBisectorPoolSize;
    EXPECT_EQ(kWQLaneCount, 3u);

    // Lane 0: Split -> PropagateBisect -> Simplify (all alias offset base + 0*P).
    EXPECT_EQ(WQSplitQueueOffset(p), kWQCounterSlots + 0u * p);
    EXPECT_EQ(WQPropagateBisectQueueOffset(p), kWQCounterSlots + 0u * p);
    EXPECT_EQ(WQSimplifyQueueOffset(p), kWQCounterSlots + 0u * p);
    // Lane 1: SimplifyClass -> PropagateSimplify (alias base + 1*P).
    EXPECT_EQ(WQSimplifyClassQueueOffset(p), kWQCounterSlots + 1u * p);
    EXPECT_EQ(WQPropagateSimplifyQueueOffset(p), kWQCounterSlots + 1u * p);
    // Lane 2: Allocate (base + 2*P).
    EXPECT_EQ(WQAllocateQueueOffset(p), kWQCounterSlots + 2u * p);

    // The three lanes must be mutually distinct (aliasing within a lane, never across).
    EXPECT_NE(WQSplitQueueOffset(p), WQSimplifyClassQueueOffset(p));
    EXPECT_NE(WQSplitQueueOffset(p), WQAllocateQueueOffset(p));
    EXPECT_NE(WQSimplifyClassQueueOffset(p), WQAllocateQueueOffset(p));

    EXPECT_EQ(WQElementCount(p), kWQCounterSlots + 3u * p);
}

// The work-queue lane offsets are hand-mirrored between C++ (CBTLayout.h WQ*Offset) and GLSL
// (cbt_layout.glsl CBT_WQ_*_QUEUE). No static_assert bridges C++ <-> GLSL, so parse each GLSL
// lane multiplier and lock it to the C++ lane (the GlslAtlasMaxTilesMatchesCpp pattern) — a future
// edit to one side without the other would silently mis-alias a queue and corrupt topology.
TEST(CBTLayout, GlslWorkQueueLanesMatchCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    const uint32_t p = kDefaultBisectorPoolSize;
    auto glslLaneMultiplier = [&](const char* name) -> uint32_t {
        std::smatch m;
        const std::regex re(std::string("const\\s+uint\\s+") + name +
                            "\\s*=\\s*CBT_WQ_COUNTER_SLOTS\\s*\\+\\s*([0-9]+)u\\s*\\*\\s*CBT_POOL_SIZE");
        EXPECT_TRUE(std::regex_search(src, m, re)) << name << " not found in cbt_layout.glsl";
        return m.empty() ? 0xFFFFFFFFu : static_cast<uint32_t>(std::stoul(m[1].str()));
    };
    auto cppLane = [&](uint32_t offset) { return (offset - kWQCounterSlots) / p; };

    EXPECT_EQ(glslLaneMultiplier("CBT_WQ_SPLIT_QUEUE"), cppLane(WQSplitQueueOffset(p)));
    EXPECT_EQ(glslLaneMultiplier("CBT_WQ_PROPAGATE_BISECT_QUEUE"), cppLane(WQPropagateBisectQueueOffset(p)));
    EXPECT_EQ(glslLaneMultiplier("CBT_WQ_SIMPLIFY_QUEUE"), cppLane(WQSimplifyQueueOffset(p)));
    EXPECT_EQ(glslLaneMultiplier("CBT_WQ_SIMPLIFY_CLASS_QUEUE"), cppLane(WQSimplifyClassQueueOffset(p)));
    EXPECT_EQ(glslLaneMultiplier("CBT_WQ_PROPAGATE_SIMPLIFY_QUEUE"), cppLane(WQPropagateSimplifyQueueOffset(p)));
    EXPECT_EQ(glslLaneMultiplier("CBT_WQ_ALLOCATE_QUEUE"), cppLane(WQAllocateQueueOffset(p)));
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

TEST(CBTLayout, IndirectStreamLayout)
{
    EXPECT_EQ(kIndirectDispatchWords, 9u);   // uvec3 x3
    EXPECT_EQ(kIndirectDrawWords, 15u);      // 5-word VkDrawIndexedIndirectCommand x3
    EXPECT_EQ(kDrawStreamStride, 5u);
    EXPECT_EQ(IndirectDispatchSlotByteOffset(1), 12u);
    EXPECT_EQ(DrawStreamWordOffset(kDrawStreamVisible), 5u);
}

// The draw-stream SET. BisectorIndexation atomicAdds into one indexCount per stream and
// PrepareBisectorIndirect clamps all three, so both the count and the per-stream index are a
// GPU-side contract: the ALL record drives the live-set dispatch width, VISIBLE is the record the
// terrain draw consumes at its byte offset, MODIFIED feeds dispatch slot 2 and is the observable
// CBTScreenSpaceTests asserts the MODIFIED flag through. Adding or dropping a stream renumbers
// every record offset in the shared indirect-draw buffer, so it fails here first.
TEST(CBTLayout, DrawStreamSet)
{
    EXPECT_EQ(kDrawStreamCount, 3u);
    EXPECT_EQ(kDrawStreamAll, 0u);
    EXPECT_EQ(kDrawStreamVisible, 1u);
    EXPECT_EQ(kDrawStreamModified, 2u);
    EXPECT_EQ(kIndirectDrawWords, kDrawStreamCount * kDrawStreamStride);
    EXPECT_EQ(DrawStreamWordOffset(kDrawStreamAll), 0u);
    EXPECT_EQ(DrawStreamWordOffset(kDrawStreamModified), 10u);
    EXPECT_EQ(kDrawStreamAllByteOffset, 0u);
    EXPECT_EQ(kIndirectDrawStrideBytes, 20u); // sizeof(VkDrawIndexedIndirectCommand)
}

// The SoA storage-buffer SET. CBTResources creates one buffer per ordinal and both
// MakeDescriptorSetLayout and WriteDescriptorSet loop 0..kCBTBindingCount using the ordinal AS the
// descriptor binding number, so the enum value is the binding — removing an entry renumbers every
// later one on the compute set, and cbt_layout.glsl's hand-written compute-branch literals must
// move in lockstep (GlslStorageBindingNumbersMatchCpp pins that pairing). The graphics set-2 view
// is NOT renumbered: it declares only gIdxVis (binding 10) and gVertex (binding 12) at fixed
// literals, and MaterialBinder resolves those from DrawBindings by reflected instance name, not by
// CBTBinding ordinal. Pinned per entry so any such change is deliberate and pairs with a GLSL edit.
TEST(CBTLayout, StorageBindingSet)
{
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::HeapID), 0u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::NeighborsA), 1u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::NeighborsB), 2u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::BisectorData), 3u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::Bitfield), 4u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::SumTree), 5u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::WorkQueue), 6u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::IndirectDispatch), 7u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::IndirectDraw), 8u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::IndicesAll), 9u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::IndicesVisible), 10u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::IndicesModified), 11u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::CurrentVertex), 12u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::Validation), 13u);
    EXPECT_EQ(static_cast<uint32_t>(CBTBinding::Count), kCBTBindingCount);
}

// The pool-scaled persistent buffers, in bytes at the default pool. Each index stream is one u32
// per pool slot and the identity index buffer is three (one per triangle corner), so the draw-side
// index buffer alone is 3x any one stream — the figure a sizing decision has to start from.
TEST(CBTLayout, PoolScaledBufferBytesAtDefaultPool)
{
    constexpr uint32_t p = kDefaultBisectorPoolSize;
    constexpr uint64_t kIndexStreamBytes = static_cast<uint64_t>(p) * sizeof(uint32_t);
    EXPECT_EQ(kIndexStreamBytes, 4u * 1024u * 1024u); // 4 MiB per index stream, x3 streams
    EXPECT_EQ(static_cast<uint64_t>(IdentityIndexCount(p)) * sizeof(uint32_t),
              12u * 1024u * 1024u); // 12 MiB identity index buffer
    EXPECT_EQ(IdentityIndexCount(p), 3u * p);
}

// The GLSL compute-branch binding numbers are hand-written literals in cbt_layout.glsl; nothing in
// the C++ build reads them, so a renumber on one side silently misroutes every kernel access on the
// other. Parse the declarations and lock each block to its CBTBinding ordinal (the
// GlslAtlasMaxTilesMatchesCpp pattern). Only the compute branch is parsed — the graphics view
// re-declares two of the same block names against set 2.
TEST(CBTLayout, GlslStorageBindingNumbersMatchCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    const size_t computeBranch = src.find("!CBT_GRAPHICS_INCLUDE");
    ASSERT_NE(computeBranch, std::string::npos)
        << "cbt_layout.glsl no longer has a !CBT_GRAPHICS_INCLUDE compute branch";
    const std::string computeSrc = src.substr(computeBranch);

    struct BlockBinding
    {
        const char* glslBlock;
        CBTBinding cppBinding;
    };
    constexpr BlockBinding kBlocks[] = {
        {"CBTHeapIDBuffer", CBTBinding::HeapID},
        {"CBTNeighborsABuffer", CBTBinding::NeighborsA},
        {"CBTNeighborsBBuffer", CBTBinding::NeighborsB},
        {"CBTBisectorDataBuffer", CBTBinding::BisectorData},
        {"CBTBitfieldBuffer", CBTBinding::Bitfield},
        {"CBTSumTreeBuffer", CBTBinding::SumTree},
        {"CBTWorkQueueBuffer", CBTBinding::WorkQueue},
        {"CBTIndirectDispatchBuffer", CBTBinding::IndirectDispatch},
        {"CBTIndirectDrawBuffer", CBTBinding::IndirectDraw},
        {"CBTIndicesAllBuffer", CBTBinding::IndicesAll},
        {"CBTIndicesVisibleBuffer", CBTBinding::IndicesVisible},
        {"CBTIndicesModifiedBuffer", CBTBinding::IndicesModified},
        {"CBTVertexBuffer", CBTBinding::CurrentVertex},
        {"CBTValidationBuffer", CBTBinding::Validation},
    };
    static_assert(std::size(kBlocks) == kCBTBindingCount,
                  "every CBTBinding needs a GLSL block in the lock table");

    for (const BlockBinding& b : kBlocks)
    {
        const std::regex re(R"(binding\s*=\s*([0-9]+)\)[^\n]*buffer\s+)" + std::string(b.glslBlock) +
                            R"(\b)");
        std::smatch m;
        ASSERT_TRUE(std::regex_search(computeSrc, m, re))
            << b.glslBlock << " not declared in the cbt_layout.glsl compute branch";
        EXPECT_EQ(static_cast<uint32_t>(std::stoul(m[1].str())),
                  static_cast<uint32_t>(b.cppBinding))
            << "cbt_layout.glsl " << b.glslBlock << " binding != C++ CBTBinding ordinal";
    }
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

TEST(CBTLayout, PushConstantsWithinPolicy)
{
    EXPECT_LE(sizeof(CBTPushConstants), 128u); // device push-constant policy
    // 10 u32 + 4 f32 dirty rect + 2 u32 (DirtyFace, SphereSculptEnabled — planet editing) +
    // 1 u32 (GateVertexEval — planet-shading quiescence gate) + 1 u32 (EditRetessEnabled — the
    // edit-driven-retess crease-term A/B toggle, round-8b) + 1 u32 (NearFieldGate — the near-field
    // force-split occupancy gate A/B toggle, round-8e saturation-deadlock fix) + 1 u32 (RegionFreeMask
    // — the far-field-drain region-reseed root bitmask, round-9) + 1 u32 (DeepDecode — the
    // Earth-scale (sector, local) storage A/B, arc S2a; GLSL mirror pc.deepDecode) + 1 u32
    // (PoolPressure — the planar pool-pressure scale's A/B arm; GLSL mirror pc.poolPressure).
    EXPECT_EQ(sizeof(CBTPushConstants), 88u);
}

// The far-field region-reseed link kernel re-derives base root adjacency from a compile-time
// CBT_ROOT_TWIN table baked into cbt_layout.glsl (there is no per-planet upload of the twin links).
// It MUST equal the cross-face twins BuildSphereRoots computes, or a reseeded root would link to the
// wrong neighbor and Validate would flag non-reciprocity. Parse the GLSL literal and lock it to the
// CPU (the GlslAtlasMaxTilesMatchesCpp pattern — no static_assert bridges C++ <-> GLSL).
TEST(CBTLayout, GlslRootTwinTableMatchesCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    std::smatch m;
    ASSERT_TRUE(std::regex_search(
        src, m, std::regex(R"(CBT_ROOT_TWIN\s*\[\s*24\s*\]\s*=\s*uint\s*\[\s*24\s*\]\s*\(([^)]*)\))")))
        << "cbt_layout.glsl lost the CBT_ROOT_TWIN[24] initializer";
    const std::string body = m[1].str();
    std::array<uint32_t, kSphereRootCount> glsl{};
    uint32_t count = 0;
    const std::regex entryRe(R"((\d+)u)");
    for (std::sregex_iterator it(body.begin(), body.end(), entryRe), end; it != end; ++it)
    {
        ASSERT_LT(count, kSphereRootCount) << "CBT_ROOT_TWIN has more than 24 entries";
        glsl[count++] = static_cast<uint32_t>(std::stoul((*it)[1].str()));
    }
    ASSERT_EQ(count, kSphereRootCount) << "CBT_ROOT_TWIN must have exactly 24 entries";

    const auto roots = BuildSphereRoots();
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
        EXPECT_EQ(glsl[r], roots[r].Neighbors.Twin)
            << "CBT_ROOT_TWIN[" << r << "] diverged from BuildSphereRoots";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}

// CPU/GPU agreement for the per-frame ring. The CPU reduces the frame counter with
// CBTFrameRingSlot and pushes that counter UNREDUCED; the shader is the second half of
// the same reduction. Two things must therefore hold, and neither can be static_asserted
// across the C++/GLSL boundary:
//   1. the ring depths are the same number, and
//   2. every GPU ring read reduces pc.frameIndex by that depth — a read that indexed by
//      something else, or by a raw pc.frameIndex, would sample a different element than
//      the CPU wrote this frame.
TEST(CBTLayout, GlslFrameRingMirrorsCpp)
{
#if defined(CBT_SHADER_SOURCE_DIR)
    const std::string path = std::string(CBT_SHADER_SOURCE_DIR) + "/cbt_layout.glsl";
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << "cannot open " << path;
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string src = ss.str();

    std::smatch m;
    const std::regex depthRe(R"(const\s+uint\s+CBT_FRAME_RING\s*=\s*([0-9]+)u?\s*;)");
    ASSERT_TRUE(std::regex_search(src, m, depthRe)) << "CBT_FRAME_RING not found in cbt_layout.glsl";
    const uint32_t glslRing = static_cast<uint32_t>(std::stoul(m[1].str()));
    EXPECT_EQ(glslRing, kCBTFrameParamsRing)
        << "cbt_layout.glsl CBT_FRAME_RING (" << glslRing << ") != C++ kCBTFrameParamsRing ("
        << kCBTFrameParamsRing << ") — the CPU and GPU halves of CBTFrameRingSlot disagree on "
           "how many elements the ring has";

    // Strip line comments first: the file DOCUMENTS the selector in prose, and prose must
    // not be able to satisfy (or break) the count.
    std::string code = std::regex_replace(src, std::regex(R"(//[^\n]*)"), "");

    const std::regex useRe(R"(pc\.frameIndex)");
    const std::regex reducedRe(R"(pc\.frameIndex\s*%\s*CBT_FRAME_RING)");
    const auto uses = std::distance(std::sregex_iterator(code.begin(), code.end(), useRe),
                                    std::sregex_iterator());
    const auto reduced = std::distance(std::sregex_iterator(code.begin(), code.end(), reducedRe),
                                       std::sregex_iterator());
    EXPECT_GT(uses, 0) << "no GPU ring read found — the mirror lock is inert";
    EXPECT_EQ(uses, reduced)
        << uses - reduced << " of " << uses << " pc.frameIndex uses in cbt_layout.glsl are not "
           "reduced by CBT_FRAME_RING; the push constant carries an UNWRAPPED frame counter, so "
           "an unreduced use indexes past the ring or names a different element than the CPU wrote";
#else
    GTEST_SKIP() << "CBT_SHADER_SOURCE_DIR not defined";
#endif
}
