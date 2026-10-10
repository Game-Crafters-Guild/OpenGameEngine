// ShaderComposer implementation: compose adapter + surface + lighting into
// compilable GLSL source strings from a MaterialDocument and ShaderVariantKey.

#include "Rendering/Materials/ShaderComposer.h"

#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderPropertyTableCache.h"
#include "Rendering/Materials/UserKeyword.h"
#include "Rendering/ShaderGraph/SgTagParser.h"
#include "Rendering/Materials/MaterialShaderIncludeRoots.h"

#include "Logger/Logger.h"
#include "Types/NearestName.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
namespace Rendering
{

namespace
{

// Canonical well-known texture-slot ladder. MUST mirror the fixed sampler
// aliases in Adapters/adapter_forward.glsl (albedoMap=0 .. metallicMap=7) and
// the ordinals in Engine's TextureSlotFromName (Material.cpp). A declared
// well-known @texture name keeps this ordinal so existing surfaces, the fixed
// aliases, and batching identity are all preserved with zero migration.
struct WellKnownSlot
{
    const char* Name;
    uint8_t Ordinal;
};
constexpr std::array<WellKnownSlot, 14> kWellKnownSlots = {{
    {"albedoMap", 0},
    {"normalMap", 1},
    {"metallicRoughnessMap", 2},
    {"emissiveMap", 3},
    {"aoMap", 4},
    {"coatNormalMap", 5},
    {"roughnessMap", 6},
    {"metallicMap", 7},
    // triplanar front-end: albedo top/side/bottom -> 0/2/4, normal -> 1/3/5.
    {"triplanarAlbedoTop", 0},
    {"triplanarNormalTop", 1},
    {"triplanarAlbedoSide", 2},
    {"triplanarNormalSide", 3},
    {"triplanarAlbedoBottom", 4},
    {"triplanarNormalBottom", 5},
}};

// The addressable slot count (MaterialData TextureIndices[8]); user names pack
// into [0, kTextureSlotWindow). Kept in sync with Material.h kTextureSlotArraySize.
constexpr uint8_t kTextureSlotWindow = 8;

// Returns the canonical ordinal for a well-known name, or kTextureSlotWindow if
// the name is user-defined (not on the ladder).
uint8_t WellKnownSlotOrdinal(const std::string& name)
{
    for (const WellKnownSlot& s : kWellKnownSlots)
    {
        if (name == s.Name)
            return s.Ordinal;
    }
    return kTextureSlotWindow;
}

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// A surface reads a texture by RAW ordinal when it pulls in a triplanar/collapse
// helper (dynamic per-axis GE_SLOT_TEX(slot)) or writes a numeric-literal
// GE_SLOT_TEX(<n>). Such a surface must not also mint user @texture names: a
// user name packed onto ordinal 2 would silently alias "albedo axis Y" — the
// exact black-default trap the triplanar header documents. The self-contained
// GE_USER_TEXTURE(name) form expands to GE_MATERIAL_TEXTURE(GE_TEXSLOT_name)
// (a macro, never a digit, and not the GE_SLOT_TEX( token), so it does not trip
// this.
//
// Remove // line comments and /* */ block comments so textual pattern scans see
// only code. String literals don't exist in GLSL surface sources, so a plain
// state machine suffices.
std::string StripComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    for (size_t i = 0; i < source.size();)
    {
        if (source.compare(i, 2, "//") == 0)
        {
            while (i < source.size() && source[i] != '\n')
                ++i;
        }
        else if (source.compare(i, 2, "/*") == 0)
        {
            const size_t end = source.find("*/", i + 2);
            i = end == std::string::npos ? source.size() : end + 2;
            out.push_back(' '); // keep token separation
        }
        else
        {
            out.push_back(source[i]);
            ++i;
        }
    }
    return out;
}

// Known limit (user @texture surfaces are supported SELF-CONTAINED only): the
// scan is textual over the TOP-LEVEL source and does not expand #includes (a
// custom helper using raw ordinals is not caught); include-expansion is the
// follow-up if user helper libraries become a pattern. The scan runs on
// comment-stripped source — a doc comment mentioning triplanar_pbr or
// GE_SLOT_TEX(0) is prose, not usage (the unstripped scan hard-rejected the
// shipped water surface, whose header cites the triplanar doc).
bool SurfaceUsesRawOrdinals(const std::string& source)
{
    const std::string code = StripComments(source);
    if (code.find("GE_SampleTriplanarSlot") != std::string::npos)
        return true;
    if (code.find("triplanar_pbr") != std::string::npos)
        return true;
    // GE_SLOT_TEX( <optional spaces> <digit>  — a raw numeric ordinal.
    const std::string token = "GE_SLOT_TEX(";
    for (size_t at = code.find(token); at != std::string::npos; at = code.find(token, at + 1))
    {
        size_t i = at + token.size();
        while (i < code.size() && (code[i] == ' ' || code[i] == '\t'))
            ++i;
        if (i < code.size() && std::isdigit(static_cast<unsigned char>(code[i])))
            return true;
    }
    return false;
}

// GLSL float literal with a decimal point, so a constant-folded default never
// turns into an int expression.
std::string GlslFloat(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    std::string s(buf);
    if (s.find_first_of(".eEn") == std::string::npos)
        s += ".0";
    return s;
}

std::string GlslConstant(const ShaderProperty& p)
{
    const uint32_t n = p.ComponentCount();
    switch (p.Type)
    {
    case ShaderPropertyType::Bool:
        return p.Default[0] != 0.0f ? "true" : "false";
    case ShaderPropertyType::Int:
    case ShaderPropertyType::Enum:
        return std::to_string(static_cast<int>(p.Default[0]));
    default:
        break;
    }
    if (n == 1)
        return GlslFloat(p.Default[0]);
    std::string out = std::string(p.GlslType()) + "(";
    for (uint32_t i = 0; i < n; ++i)
        out += (i ? ", " : "") + GlslFloat(p.Default[i]);
    return out + ")";
}

std::string GlslLaneRead(const ShaderProperty& p, const std::string& laneExpr)
{
    switch (p.Type)
    {
    case ShaderPropertyType::Bool:
        return "(" + laneExpr + " != 0.0)";
    case ShaderPropertyType::Int:
    case ShaderPropertyType::Enum:
        return "floatBitsToInt(" + laneExpr + ")";
    default:
        return laneExpr;
    }
}

std::string SwizzleFor(uint32_t component, uint32_t count)
{
    return std::string("xyzw").substr(component, count);
}

// The lane an adapter read binds to on a surface that carries no declarations
// of its own and is still wired through MaterialRegistry's hand-typed table (the
// engine surfaces, until each declares its properties). Both sides read the one
// placement table in Materials/LegacyMaterialLanes.h, so the composer cannot
// drift from the offsets the registry writes.
std::optional<std::string> LegacyAdapterLaneFor(const std::string& name)
{
    if (const LegacyMaterialLane* lane = FindLegacyMaterialLane(name))
        return LegacyMaterialLaneGlsl(*lane);
    return std::nullopt;
}

// `Mat.<declaredName>` in a declared surface: the author reached for the raw
// row where the declared accessor is the contract. One error per offending
// line, attributed to the surface file.
void DiagnoseMatReadsOfDeclaredNames(const std::string& surfaceSource, const std::string& surfaceFile,
                                     const ShaderPropertyTable& table,
                                     std::vector<ShaderPropertyDiagnostic>& errors)
{
    std::istringstream iss(surfaceSource);
    std::string line;
    uint32_t lineNo = 0;
    while (std::getline(iss, line))
    {
        ++lineNo;
        const size_t comment = line.find("//");
        const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
        for (const ShaderProperty& p : table.Properties)
        {
            if (p.Origin == ShaderPropertyOrigin::Adapter && !p.HasLane)
                continue;
            const std::string token = "Mat." + p.Name;
            for (size_t at = code.find(token); at != std::string::npos; at = code.find(token, at + 1))
            {
                const size_t end = at + token.size();
                if (end < code.size() && IsIdentChar(code[end]))
                    continue;
                errors.push_back({surfaceFile, lineNo,
                                  "'" + p.Name + "' is a declared property: read it as Props." + p.Name +
                                      ", not Mat." + p.Name});
                break;
            }
        }
    }
}

bool ReadsDeclaredName(const std::string& code, const std::string& token)
{
    for (size_t at = code.find(token); at != std::string::npos; at = code.find(token, at + 1))
    {
        const size_t end = at + token.size();
        if (end >= code.size() || !IsIdentChar(code[end]))
            return true;
    }
    return false;
}

// A declared property the program never reads is legal (a helper include may
// read it) but almost always a leftover or a typo, so it is a warning. The
// program includes the adapters: a surface that redeclares an adapter read
// (alphaCutoff, to expose the slider) never spells Props.alphaCutoff itself.
std::vector<std::string> DiagnoseUnreadDeclaredProperties(const ShaderPropertyTable& table,
                                                          const std::string& surfaceCode,
                                                          const std::string& modifierCode,
                                                          const std::string& adapterCode)
{
    std::vector<std::string> lines;
    for (const ShaderProperty& p : table.Properties)
    {
        if (p.Origin == ShaderPropertyOrigin::Adapter)
            continue;
        const std::string& code = p.Origin == ShaderPropertyOrigin::Surface ? surfaceCode : modifierCode;
        const std::string token = "Props." + p.Name;
        if (!ReadsDeclaredName(code, token) && !ReadsDeclaredName(adapterCode, token))
            lines.push_back(p.SourceFile + ":" + std::to_string(p.SourceLine) + ": warning: declared property '" +
                            p.Name + "' is never read (no Props." + p.Name +
                            " in this file; reads from an #include are not detected)");
    }
    return lines;
}

static bool ReadFile(const std::filesystem::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// Quote a resolved shader path as a literal #include argument. The composed
// source is a cache key input, so an absolute spelling here would pin every
// compiled variant to the machine that composed it.
//
// Prefer the authored relative reference when the document already named one
// (`Surfaces/ez_tree_leaves.glsl`). Reconstructing that spelling from the
// resolved absolute path via deepest-root arithmetic fails on some runtimes
// (MEMFS path equality) and then emits an absolute #include — intern hashes a
// different key than the offline cook, and a compiler-less runtime reports
// "no cooked program" for a row that is sitting in the cache.
static std::string MakeIncludeLiteral(const std::filesystem::path& resolved,
                                      const std::vector<std::filesystem::path>& includeRoots,
                                      std::string_view authoredRelative = {})
{
    if (!authoredRelative.empty())
    {
        const std::filesystem::path authored{std::string(authoredRelative)};
        if (!authored.empty() && !authored.is_absolute())
        {
            // Only emit the intern-stable spelling when an include root actually
            // serves that file as `resolved`. Otherwise shaderc cannot find it
            // and compiles an empty EvaluateSurface.
            std::error_code ec;
            const std::filesystem::path target =
                std::filesystem::absolute(resolved, ec).lexically_normal();
            for (const auto& root : includeRoots)
            {
                const std::filesystem::path cand =
                    (std::filesystem::absolute(root, ec) / authored).lexically_normal();
                if (std::filesystem::exists(cand, ec) && cand == target)
                    return "\"" + authored.generic_string() + "\"";
            }
        }
    }

    std::filesystem::path shadowedBy;
    const std::string spelling =
        MakeRelocatableIncludeSpelling(resolved, includeRoots, &shadowedBy);
    if (!shadowedBy.empty())
    {
        Logger::Log::Warning(
            "ShaderComposer: '{}' cannot be named relative to any include root — '{}' shadows it "
            "under the same spelling. Falling back to an absolute #include: this variant's cache "
            "entry is valid only on this machine. Rename one of the two files.",
            resolved.generic_string(), shadowedBy.generic_string());
    }
    return "\"" + spelling + "\"";
}

struct AdapterSelection
{
    const char* vertexFile = nullptr;
    const char* fragmentFile = nullptr;
};

static AdapterSelection SelectAdapters(const ShaderVariantKey& /*key*/)
{
    AdapterSelection sel{};
    sel.vertexFile = "Adapters/adapter_vertex.glsl";
    sel.fragmentFile = "Adapters/adapter_forward.glsl";
    return sel;
}

static bool IsGraphLivePreviewMaterialDirectory(const std::filesystem::path& materialDir)
{
    return !materialDir.empty() && materialDir.filename() == "GraphPreview";
}

// Human-readable list of every root ResolveShaderReference consults, in probe
// order, for unresolved-surface diagnostics.
static std::string DescribeSearchedShaderRoots(const std::filesystem::path& materialDir,
                                               const MaterialBuildContext& context)
{
    std::ostringstream oss;
    oss << "material dir "
        << (materialDir.empty() ? std::string("<unavailable>")
                                : "'" + materialDir.generic_string() + "'");
    oss << ", project root(s) [";
    for (size_t i = 0; i < context.ProjectRoots.size(); ++i)
        oss << (i ? ", " : "") << "'" << context.ProjectRoots[i].generic_string() << "'";
    oss << "], package shader dir(s) [";
    for (size_t i = 0; i < context.PackageShaderDirs.size(); ++i)
        oss << (i ? ", " : "") << "'" << context.PackageShaderDirs[i].generic_string() << "'";
    oss << "], engine shader dir '" << context.AdapterShaderDir.generic_string() << "'";
    return oss.str();
}

// Merge the surface-derived and graph variant defines into `defines`. Takes the
// already-read surface source (Compose reads it once) so the tag scan, the
// texture-slot resolution, and the user-keyword emission all share one read.
static void MergeSurfaceShaderVariantDefines(std::vector<std::string>& defines,
                                             const MaterialDocument& doc,
                                             const std::string& surfaceSource,
                                             const std::filesystem::path& materialDir)
{
    auto addDefine = [&](const std::string& name)
    {
        if (name.empty())
            return;
        if (std::find(defines.begin(), defines.end(), name) == defines.end())
            defines.push_back(name);
    };

    // Live preview draws on tangent-capable primitives; keep the shader variant
    // aligned with the mesh even when the graph only uses world-space normals.
    if (IsGraphLivePreviewMaterialDirectory(materialDir))
        addDefine("HAS_TANGENT");

    if (doc.surfaceShader.find("terrain_grass_surface.glsl") != std::string::npos)
        addDefine("GE_TWO_SIDED_KEEP_NORMAL");

    if (ShaderGraph::IsShaderGraphSource(surfaceSource))
    {
        const auto parsed = ShaderGraph::ParseShaderGraphSource(surfaceSource);
        const auto graphDoc = ShaderGraph::ParseGraphDocumentFromTags(parsed.TagBlock);
        for (const std::string& def : graphDoc.VariantDefines)
            addDefine(def);
    }

    // User keywords (§2): emit GE_USER_<NAME> flag defines. The names arrive
    // already sanitized (SanitizeUserKeyword at parse) but the preamble is raw
    // textual injection, so re-sanitize at this boundary — an unsanitized name
    // is a shader-injection footgun, and GE_USER_ namespacing keeps a user
    // keyword from ever clobbering an engine define.
    for (const std::string& kw : doc.keywords)
    {
        if (const auto sanitized = SanitizeUserKeyword(kw))
            addDefine("GE_USER_" + *sanitized);
    }

    // Both sides of the keyword contract are visible here: the names the material
    // enables and the source that either reads them or does not. A keyword the
    // surface never reads is legal but almost always a typo, and it is otherwise
    // completely silent.
    for (const std::string& line :
         ShaderComposer::DiagnoseUnreadKeywords(surfaceSource, doc.keywords, doc.surfaceShader))
        Logger::Log::Warning("{}", line);
}

} // namespace

std::string ShaderComposer::GenerateDefinePreamble(const ShaderVariantKey& key,
                                                    const std::string& lightingModelName)
{
    return GenerateDefinePreamble(GenerateDefines(key, lightingModelName));
}

std::string ShaderComposer::GenerateDefinePreamble(const std::vector<std::string>& defines)
{
    std::ostringstream oss;
    for (const auto& d : defines)
    {
        oss << "#define " << d << "\n";
    }
    return oss.str();
}

std::string ShaderComposer::GenerateDeclaredPropertiesBlock(const ShaderPropertyTable& table)
{
    std::ostringstream oss;
    oss << "// --- ShaderComposer declared properties ---\n";
    oss << "struct GE_Props\n{\n";
    for (const ShaderProperty& p : table.Properties)
        oss << "    " << p.GlslType() << " " << p.Name << ";\n";
    oss << "};\n";
    oss << "GE_Props ge_Props;\n";
    oss << "#define Props ge_Props\n";
    oss << "void GE_LoadDeclaredProperties()\n{\n";
    for (const ShaderProperty& p : table.Properties)
    {
        std::string value;
        if (p.HasLane)
        {
            std::string lane = "ge_MatData.uParams[" + std::to_string(p.Lane) + "]";
            if (p.ComponentCount() < 4)
                lane += "." + SwizzleFor(p.Component, p.ComponentCount());
            value = GlslLaneRead(p, lane);
        }
        else if (const std::optional<std::string> legacy =
                     table.HasSurfaceDeclarations ? std::nullopt : LegacyAdapterLaneFor(p.Name))
        {
            value = GlslLaneRead(p, "ge_MatData." + *legacy);
        }
        else
        {
            value = GlslConstant(p);
        }
        oss << "    ge_Props." << p.Name << " = " << value << ";\n";
    }
    oss << "}\n";
    return oss.str();
}

std::vector<std::string> ShaderComposer::DiagnoseUnreadKeywords(
    const std::string& surfaceSource, const std::vector<std::string>& keywords,
    const std::string& surfaceName)
{
    std::vector<std::string> lines;
    if (keywords.empty())
        return lines;

    // Every GE_USER_<NAME> the surface mentions, comment-stripped so a doc
    // comment quoting a keyword is prose rather than a consumer. GE_USER_TEXTURE
    // is the named-slot accessor macro, not a keyword — SanitizeUserKeyword
    // reserves TEXTURE for exactly this reason, so it can never match anyway.
    static constexpr std::string_view kPrefix = "GE_USER_";
    std::vector<std::string> read;
    const std::string code = StripComments(surfaceSource);
    for (size_t at = code.find(kPrefix); at != std::string::npos;
         at = code.find(kPrefix, at + 1))
    {
        // A prefix that continues an identifier (FOO_GE_USER_BAR) is not a use.
        if (at > 0 && IsIdentChar(code[at - 1]))
            continue;
        size_t end = at + kPrefix.size();
        while (end < code.size() && IsIdentChar(code[end]))
            ++end;
        std::string name = code.substr(at + kPrefix.size(), end - at - kPrefix.size());
        if (name.empty() || name == "TEXTURE")
            continue;
        if (std::find(read.begin(), read.end(), name) == read.end())
            read.push_back(std::move(name));
    }

    const std::string surface = surfaceName.empty() ? std::string("<default surface>") : surfaceName;
    for (const std::string& kw : keywords)
    {
        const auto sanitized = SanitizeUserKeyword(kw);
        if (!sanitized)
            continue; // rejected at emission too — not an "unread keyword" case
        if (std::find(read.begin(), read.end(), *sanitized) != read.end())
            continue;

        std::string line = "ShaderComposer: material keyword '" + kw + "' is never read by surface '" +
                           surface + "' (no #ifdef " + std::string(kPrefix) + *sanitized + ")";
        line += NearestNameSuffix(*sanitized, read);
        if (read.empty())
            line += "; that surface reads no GE_USER_ keyword at all — keywords read from an "
                    "#include are not detected here";
        lines.push_back(std::move(line));
    }
    return lines;
}

TextureSlotResolution ShaderComposer::ResolveTextureSlots(const std::string& surfaceSource)
{
    TextureSlotResolution result{};

    // 1. Scan `// @texture <name> [srgb|linear]` comment tags for declared names.
    std::vector<std::string> declared; // declaration order, deduped
    {
        std::istringstream iss(surfaceSource);
        std::string line;
        while (std::getline(iss, line))
        {
            const size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos || line.compare(s, 2, "//") != 0)
                continue; // the tag lives in a line comment
            // The tag must be the FIRST token after "//" — a mid-prose mention
            // (e.g. a doc comment quoting `// @texture flowMask linear` as an
            // example) must not mint a phantom user name. That false positive
            // hard-rejected every ER water compose once the K validation landed.
            size_t at = s + 2;
            while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
                ++at;
            if (line.compare(at, 8, "@texture") != 0)
                continue;
            size_t i = at + 8; // past "@texture"
            if (i < line.size() && line[i] != ' ' && line[i] != '\t')
                continue; // "@textureX" is not the tag
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;
            if (i >= line.size() || !IsIdentStart(line[i]))
                continue;
            const size_t nameStart = i;
            while (i < line.size() && IsIdentChar(line[i]))
                ++i;
            std::string name = line.substr(nameStart, i - nameStart);
            // The optional trailing srgb|linear color-space token is part of the
            // declared grammar but consumed by a later slice (per-texture
            // colorSpace override); the slot map ignores it here.
            if (std::find(declared.begin(), declared.end(), name) == declared.end())
                declared.push_back(std::move(name));
        }
    }

    if (declared.empty())
        return result; // legacy surface — HasDeclarations stays false, ladder used

    result.HasDeclarations = true;

    // 2. Well-known names take their canonical ordinal; collect user names.
    std::array<bool, kTextureSlotWindow> claimed{};
    std::vector<std::string> userNames;
    for (const std::string& name : declared)
    {
        const uint8_t ord = WellKnownSlotOrdinal(name);
        if (ord < kTextureSlotWindow)
            claimed[ord] = true;
        else
            userNames.push_back(name);
    }

    // 3. Reject user names mixed with raw-ordinal access (silent-alias trap).
    if (!userNames.empty() && SurfaceUsesRawOrdinals(surfaceSource))
    {
        result.Rejected = true;
        result.RejectReason =
            "surface declares user @texture name(s) but also reads textures by raw "
            "ordinal (triplanar/collapse helper or numeric GE_SLOT_TEX). A user slot "
            "would silently alias a raw ordinal; keep the surface self-contained or "
            "use only well-known names.";
        return result;
    }

    // 4. Pack user names into the lowest unclaimed ordinals, lexicographic order.
    std::sort(userNames.begin(), userNames.end());
    std::unordered_map<std::string, uint8_t> userToOrdinal;
    for (const std::string& name : userNames)
    {
        uint8_t ord = kTextureSlotWindow;
        for (uint8_t i = 0; i < kTextureSlotWindow; ++i)
        {
            if (!claimed[i])
            {
                ord = i;
                break;
            }
        }
        if (ord >= kTextureSlotWindow)
        {
            result.Rejected = true;
            result.RejectReason = "surface declares more @texture slots than the " +
                                  std::to_string(kTextureSlotWindow) + "-slot window allows.";
            return result;
        }
        claimed[ord] = true;
        userToOrdinal[name] = ord;
    }

    // 5. Emit declared slots in declaration order (stable macro emission).
    result.DeclaredSlots.reserve(declared.size());
    for (const std::string& name : declared)
    {
        const uint8_t wk = WellKnownSlotOrdinal(name);
        const uint8_t ord = (wk < kTextureSlotWindow) ? wk : userToOrdinal[name];
        result.DeclaredSlots.emplace_back(name, ord);
    }
    return result;
}

std::filesystem::path ShaderComposer::ResolveShaderReference(const std::string& relativePath,
                                                             const std::filesystem::path& materialDir,
                                                             const MaterialBuildContext& context)
{
    namespace fs = std::filesystem;
    fs::path p(relativePath);
    if (p.is_absolute())
        return fs::absolute(p);

    // An empty materialDir means the material's asset path is UNAVAILABLE
    // (cache-driven compile that could not recover it) — never a real root.
    // It is skipped entirely so a stand-in directory can't masquerade as the
    // material's own and shadow the project/package/engine chain.
    std::error_code ec;
    if (!materialDir.empty())
    {
        const fs::path fromMaterial = (materialDir / p).lexically_normal();
        if (fs::exists(fromMaterial, ec))
            return fs::absolute(fromMaterial);
    }

    for (const fs::path& projectRoot : context.ProjectRoots)
    {
        const fs::path fromProject = (projectRoot / p).lexically_normal();
        if (fs::exists(fromProject, ec))
            return fs::absolute(fromProject);
    }

    for (const fs::path& packageDir : context.PackageShaderDirs)
    {
        const fs::path fromPackage = (packageDir / p).lexically_normal();
        if (fs::exists(fromPackage, ec))
            return fs::absolute(fromPackage);
    }

    const fs::path fromAdapter = (context.AdapterShaderDir / p).lexically_normal();
    if (fs::exists(fromAdapter, ec))
        return fs::absolute(fromAdapter);

    // Nowhere found: name a concrete candidate for the caller's diagnostics —
    // the materialDir one when a material dir exists, the engine-tree one
    // otherwise.
    const fs::path missBase = materialDir.empty() ? context.AdapterShaderDir : materialDir;
    return fs::absolute((missBase / p).lexically_normal());
}

std::filesystem::path ShaderComposer::ResolveSurfaceShaderPath(const std::string& surfaceShader,
                                                               const std::filesystem::path& materialDir,
                                                               const MaterialBuildContext& context)
{
    const std::string relativePath =
        surfaceShader.empty() ? std::string("Surfaces/standard_surface.glsl") : surfaceShader;
    return ResolveShaderReference(relativePath, materialDir, context);
}

std::string ShaderComposer::ShaderRootRelative(const std::filesystem::path& path,
                                               const std::filesystem::path& materialDir,
                                               const MaterialBuildContext& context)
{
    std::filesystem::path rel;
    auto under = [&](const std::filesystem::path& root)
    {
        rel = RelativePathUnderRoot(path, root);
        return !rel.empty();
    };
    if (under(materialDir))
        return rel.generic_string();
    for (const std::filesystem::path& projectRoot : context.ProjectRoots)
        if (under(projectRoot))
            return rel.generic_string();
    for (const std::filesystem::path& packageDir : context.PackageShaderDirs)
        if (under(packageDir))
            return rel.generic_string();
    if (under(context.AdapterShaderDir))
        return rel.generic_string();
    return path.lexically_normal().filename().generic_string();
}

ComposedShaderSource ShaderComposer::Compose(const MaterialDocument& doc,
                                              const ShaderVariantKey& key,
                                              const std::filesystem::path& materialDir,
                                              const MaterialBuildContext& context,
                                              std::vector<std::string>* outErrors)
{
    const std::filesystem::path& adapterShaderDir = context.AdapterShaderDir;
    ComposedShaderSource result{};
    result.defines = GenerateDefines(key, doc.lightingModel.empty() ? "Unlit" : doc.lightingModel);

    auto addError = [&](const std::string& msg)
    {
        if (outErrors)
            outErrors->push_back(msg);
    };

    // Resolve + read the surface source ONCE and derive everything
    // source-dependent from it: the surface/graph variant defines, the
    // named-texture-slot map, and the include literal substituted below.
    const bool surfaceAuthored = !doc.surfaceShader.empty();
    const std::filesystem::path resolvedSurfacePath =
        ResolveSurfaceShaderPath(doc.surfaceShader, materialDir, context);
    std::string surfaceSource;
    const bool haveSurface = ReadFile(resolvedSurfacePath, surfaceSource);

    // An AUTHORED surface that cannot be resolved is a HARD compose failure.
    // Composing anyway would either error late inside shaderc (phantom
    // include) or silently render the default surface — the project-portability
    // trap where a copied project's materials lose their shaders while
    // everything still "works". The default surface is reserved for materials
    // that author no surface at all.
    if (surfaceAuthored && !haveSurface)
    {
        const std::string materialName = doc.materialName.empty() ? "<unnamed>" : doc.materialName;
        const std::string searched = DescribeSearchedShaderRoots(materialDir, context);
        addError("ShaderComposer: material '" + materialName + "': authored surfaceShader '" +
                 doc.surfaceShader + "' did not resolve to a readable file. Searched " + searched +
                 ". Compose FAILED — an authored surface never falls back to the default surface.");
        Logger::Log::Error(
            "ShaderComposer: material '{}': authored surfaceShader '{}' UNRESOLVED — searched {}. "
            "Compose FAILED; fix the surface path/staging (no silent default-surface fallback).",
            materialName, doc.surfaceShader, searched);
        return result; // invalid (no sources) — the build fails loudly
    }
    if (surfaceAuthored)
    {
        Logger::Log::Debug("ShaderComposer: surface '{}' -> '{}'", doc.surfaceShader,
                           resolvedSurfacePath.generic_string());
    }
    if (haveSurface)
        MergeSurfaceShaderVariantDefines(result.defines, doc, surfaceSource, materialDir);

    // Either modifier form deforms vertices. The vertex adapter guards the InstanceData
    // clock fields on this one define, so a variant without a modifier composes exactly
    // the rigid path. Derived from the merged list rather than the key: a materialized
    // graph reaches HAS_VERTEX_MODIFIER through its @sg-variant tag, never the key.
    {
        const auto hasDefine = [&](const char* name)
        {
            return std::find(result.defines.begin(), result.defines.end(), name) !=
                   result.defines.end();
        };
        if (hasDefine("HAS_VERTEX_MODIFIER") || hasDefine("HAS_VERTEX_OUTPUT_MODIFIER"))
            result.defines.push_back("GE_VERTEX_DEFORMATION");
    }

    // Named texture slots: resolve the surface's `@texture` declarations to
    // ordinals and emit a `#define GE_TEXSLOT_<name> <ordinal>` for each. These
    // are VALUED (and GE_USER_TEXTURE is function-like), so they ride the source
    // preamble only — never the `defines` list, which also feeds shaderc as bare
    // -D flags. A surface that mixes user names with raw-ordinal access is a hard
    // compose failure, not a silent misroute.
    std::string textureSlotPreamble;
    if (haveSurface)
    {
        const TextureSlotResolution slots = ResolveTextureSlots(surfaceSource);
        if (slots.Rejected)
        {
            addError("ShaderComposer: surface '" + doc.surfaceShader + "': " + slots.RejectReason);
            Logger::Log::Error("ShaderComposer: surface '{}' REJECTED: {}", doc.surfaceShader,
                               slots.RejectReason);
            return result; // invalid (no sources) — the build fails loudly
        }
        std::ostringstream tex;
        for (const auto& [name, ordinal] : slots.DeclaredSlots)
            tex << "#define GE_TEXSLOT_" << name << " " << static_cast<int>(ordinal) << "\n";
        textureSlotPreamble = tex.str();
    }

    // Select adapter template files.
    const auto adapters = SelectAdapters(key);
    if (!adapters.vertexFile || !adapters.fragmentFile)
    {
        addError("ShaderComposer: failed to select adapter templates.");
        return result;
    }

    result.vertexAdapterName = adapters.vertexFile;
    result.fragmentAdapterName = adapters.fragmentFile;

    // Resolve paths.
    const std::filesystem::path vsPath = adapterShaderDir / adapters.vertexFile;
    const std::filesystem::path fsPath = adapterShaderDir / adapters.fragmentFile;

    // The roots the compile request will hand the includer — every substituted
    // literal below is spelled against these, so composition is reproducible on
    // any machine that has the same content.
    const std::vector<std::filesystem::path> includeRoots =
        BuildMaterialIncludeRoots(context, materialDir);

    // Read adapter vertex source.
    std::string vsSrc;
    if (!ReadFile(vsPath, vsSrc))
    {
        addError("ShaderComposer: could not read vertex adapter: " + vsPath.string());
        return result;
    }

    // Read adapter fragment source.
    std::string fsSrc;
    if (!ReadFile(fsPath, fsSrc))
    {
        addError("ShaderComposer: could not read fragment adapter: " + fsPath.string());
        return result;
    }

    // Declared properties: one parse shared with registration (the cache), the
    // GE_Props block substituted into both adapters. Every declaration error is a
    // compose failure carrying file:line, so it reaches the Shader Errors panel
    // and the inspector's diagnostics; the material keeps its last good shader.
    const std::filesystem::path vertexModifierPath =
        doc.vertexModifier.empty() ? std::filesystem::path{}
                                   : ResolveShaderReference(doc.vertexModifier, materialDir, context);
    std::shared_ptr<const ShaderPropertyTable> declared = ShaderPropertyTableCache::Instance().Resolve(
        ShaderPropertyTableInputs{fsPath, haveSurface ? resolvedSurfacePath : std::filesystem::path{},
                                  vertexModifierPath});
    std::vector<ShaderPropertyDiagnostic> declarationErrors = declared->Errors;
    if (declarationErrors.empty() && haveSurface && declared->HasSurfaceDeclarations)
        DiagnoseMatReadsOfDeclaredNames(surfaceSource, resolvedSurfacePath.generic_string(), *declared,
                                        declarationErrors);
    if (!declarationErrors.empty())
    {
        std::string message = "ShaderComposer: surface '" + doc.surfaceShader + "' has " +
                              std::to_string(declarationErrors.size()) + " property declaration error(s):";
        for (const ShaderPropertyDiagnostic& e : declarationErrors)
            message += "\n" + e.Format("error");
        addError(message);
        Logger::Log::Error("{}", message);
        return result; // invalid (no sources) — the build fails loudly
    }
    for (const ShaderPropertyDiagnostic& w : declared->Warnings)
        Logger::Log::Warning("ShaderComposer: {}", w.Format("warning"));
    if (haveSurface && declared->HasSurfaceDeclarations)
    {
        std::string modifierSource;
        if (!vertexModifierPath.empty())
            ReadFile(vertexModifierPath, modifierSource);
        for (const std::string& line : DiagnoseUnreadDeclaredProperties(
                 *declared, StripComments(surfaceSource), StripComments(modifierSource),
                 StripComments(vsSrc) + StripComments(fsSrc)))
            Logger::Log::Warning("ShaderComposer: {}", line);
    }
    result.declaredProperties = declared;

    // The marker sits at a known line of the raw adapter; a #line after the
    // block keeps every later adapter diagnostic on its true line.
    const std::string declaredBlock = GenerateDeclaredPropertiesBlock(*declared);
    auto substituteDeclaredBlock = [&](std::string& src, const std::filesystem::path& adapterPath, bool required)
    {
        static constexpr std::string_view kMarker = "// GE_DECLARED_PROPERTIES";
        const size_t at = src.find(kMarker);
        if (at == std::string::npos)
        {
            if (required)
                addError("ShaderComposer: adapter '" + adapterPath.generic_string() +
                         "' has no '// GE_DECLARED_PROPERTIES' marker; declared properties were NOT emitted.");
            return !required;
        }
        const uint32_t markerLine = 1 + static_cast<uint32_t>(std::count(src.begin(), src.begin() + at, '\n'));
        // The bare file name: the composed text is a cache key, so the anchor
        // must not depend on the machine (an absolute path) or on the include
        // roots a host configures (a root-relative spelling) — the offline cook
        // and a compiler-less runtime have to agree byte for byte.
        src.replace(at, kMarker.size(),
                    declaredBlock + "#line " + std::to_string(markerLine + 1) + " \"" +
                        adapterPath.filename().generic_string() + "\"");
        return true;
    };
    if (!substituteDeclaredBlock(fsSrc, fsPath, true))
        return result;
    substituteDeclaredBlock(vsSrc, vsPath, false);

    // Generate the define preamble. The valued GE_TEXSLOT_* macros follow the
    // flag defines so the surface include (and GE_USER_TEXTURE) see them.
    const std::string preamble = GenerateDefinePreamble(result.defines) + textureSlotPreamble;

    // Inject the define preamble after the #version line, then re-anchor
    // diagnostics with a cpp-style #line directive (shaderc/glslang accept the
    // string-filename form without any extension pragma). The injection shifts
    // every subsequent line, and shaderc only knows the composed source by a
    // logical path that never exists on disk — without the anchor, a top-level
    // compile error reports "<phantom composed path>:<line+offset>". With it,
    // the error reports the REAL adapter file and its true line. Errors inside
    // the surface / vertex-modifier includes already attribute correctly: the
    // includer names each include by its resolved absolute path.
    //
    // The anchor names the adapter root-relatively: it lands in the composed
    // source, which is a cache key input, and an absolute spelling would make
    // the key machine-specific for a purely diagnostic string.
    auto injectPreamble = [&](std::string& src, const std::filesystem::path& adapterPath,
                              const char* stageDefine)
    {
        const std::string anchor = MakeRelocatableIncludeSpelling(adapterPath, includeRoots);
        const std::string block = "\n// --- ShaderComposer injected defines ---\n#define " +
                                  std::string(stageDefine) + "\n" + preamble;
        const size_t versionEnd = src.find('\n');
        if (versionEnd != std::string::npos && src.substr(0, 8) == "#version")
        {
            // The line after #version was line 2 of the original adapter file.
            src.insert(versionEnd + 1, block + "\n#line 2 \"" + anchor + "\"\n");
        }
        else
        {
            src = "#version 450\n" + block + "\n#line 1 \"" + anchor + "\"\n" + src;
        }
    };

    /* GE_STAGE_VERTEX / GE_STAGE_FRAGMENT let one source file serve both stages:
       a materialized shader graph carries EvaluateSurface and ModifyVertex in the
       same file and guards each behind the stage it belongs to. */
    injectPreamble(vsSrc, vsPath, "GE_STAGE_VERTEX");
    injectPreamble(fsSrc, fsPath, "GE_STAGE_FRAGMENT");

    // Substitute the surface shader include in the fragment source. An empty
    // surfaceShader defaults to the engine's built-in standard surface shader —
    // shaderc cannot rely on the #ifndef/#define macro fallback inside
    // adapter_forward.glsl because it requires a literal string in #include, so
    // the composer must always substitute a concrete path here. Authored
    // surfaces were validated above (hard failure when unresolved), so the
    // literal always names a readable file for them.
    {
        const std::string marker = "#include GE_SURFACE_SHADER_PATH";
        const size_t at = fsSrc.find(marker);
        if (at != std::string::npos)
        {
            fsSrc.replace(at, marker.size(),
                          "#include " + MakeIncludeLiteral(resolvedSurfacePath, includeRoots,
                                                           doc.surfaceShader));
        }
        else if (surfaceAuthored)
        {
            addError("ShaderComposer: adapter fragment has no '#include GE_SURFACE_SHADER_PATH' "
                     "marker; authored surfaceShader '" + doc.surfaceShader + "' was NOT substituted.");
            Logger::Log::Error("ShaderComposer: no GE_SURFACE_SHADER_PATH marker in the fragment "
                               "adapter; authored surfaceShader '{}' NOT substituted (default surface used)",
                               doc.surfaceShader);
        }
    }

    // Resolve the vertex modifier include in the vertex source. An authored
    // vertexModifier wins. Without one, a materialized shader graph that emitted
    // ModifyVertex (its HAS_VERTEX_MODIFIER variant define reached `defines`
    // above) is its own vertex modifier: the same file, stage-guarded inside.
    {
        const bool graphVertexModifier =
            doc.vertexModifier.empty() && haveSurface && ShaderGraph::IsShaderGraphSource(surfaceSource) &&
            std::find(result.defines.begin(), result.defines.end(), "HAS_VERTEX_MODIFIER") !=
                result.defines.end();
        const std::filesystem::path& modifierIncludePath =
            graphVertexModifier ? resolvedSurfacePath : vertexModifierPath;

        const std::string marker = "#include GE_VERTEX_MODIFIER_PATH";
        size_t at = vsSrc.find(marker);
        if (!modifierIncludePath.empty() && at != std::string::npos)
        {
            const std::string includeDirective =
                "#include " + MakeIncludeLiteral(modifierIncludePath, includeRoots,
                                                 graphVertexModifier ? doc.surfaceShader : doc.vertexModifier);
            while (at != std::string::npos)
            {
                vsSrc.replace(at, marker.size(), includeDirective);
                at = vsSrc.find(marker, at + includeDirective.size());
            }
        }
    }

    result.vertexSource = std::move(vsSrc);
    result.fragmentSource = std::move(fsSrc);
    return result;
}

} // namespace Rendering
} // namespace GameEngine
