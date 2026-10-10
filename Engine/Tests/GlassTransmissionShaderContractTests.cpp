// Source contracts for the two halves of the refractive-dielectric path that a shader
// refactor can break with no compile error and no warning on any surface:
//
//  1. Transmission splits the diffuse budget in BOTH lighting paths. ibl.glsl scales the
//     ambient diffuse by (1 - transmissionWeight); standard_pbr.glsl must scale the analytic
//     diffuse by the same factor. With only the ambient half, a weight-1 glass still shades a
//     full opaque Lambert lobe from every punctual light, which under a bright key buries the
//     refracted background and its Beer-Lambert tint — the lobe looks disconnected while every
//     keyword, parameter and code path is in fact live.
//
//  2. The glass caustic is a receiver term. It must go through the receiver's own diffuse
//     albedo (a concentration of that surface's response, not raw radiance), and a refractive
//     surface must not receive its own dapple — it is the lens, and it sits inside its own
//     glass-tint cascade, so an ungated web paints itself across the glass.
//
// Reads the repo shader source via GE_RENDERER_REPO_ROOT (dev-only anchor, same precedent as
// IblShaderContractTests): the source file is the artifact under test.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Strip //-comments so prose describing a term never counts as a use of it.
std::string StripLineComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    std::size_t pos = 0;
    while (pos < source.size())
    {
        const std::size_t comment = source.find("//", pos);
        const std::size_t lineEnd = source.find('\n', pos);
        if (comment == std::string::npos || (lineEnd != std::string::npos && comment > lineEnd))
        {
            if (lineEnd == std::string::npos)
            {
                out.append(source, pos, std::string::npos);
                break;
            }
            out.append(source, pos, lineEnd + 1 - pos);
            pos = lineEnd + 1;
            continue;
        }
        out.append(source, pos, comment - pos);
        if (lineEnd == std::string::npos)
            break;
        out.push_back('\n');
        pos = lineEnd + 1;
    }
    return out;
}

// Function body = from the signature to the first '}' at column 0 (house shader style).
std::string ExtractFunctionBody(const std::string& source, const std::string& signature)
{
    const std::size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const std::size_t end = source.find("\n}", begin);
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end + 2 - begin);
}

// Lets a test pin a composed EXPRESSION without pinning its formatting.
std::string StripWhitespace(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    for (const char c : source)
        if (!std::isspace(static_cast<unsigned char>(c)))
            out.push_back(c);
    return out;
}

std::string LoadShaderWithoutComments(const char* relativePath)
{
#ifndef GE_RENDERER_REPO_ROOT
    (void)relativePath;
    return {};
#else
    const std::filesystem::path path =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) / "Engine/Modules/Rendering/Shaders" / relativePath;
    return StripLineComments(ReadTextFile(path));
#endif
}

constexpr const char* kCaustics = "Includes/caustics.glsl";
constexpr const char* kStandardPbr = "Includes/standard_pbr.glsl";
constexpr const char* kIbl = "Includes/ibl.glsl";
constexpr const char* kClusteredLighting = "Includes/clustered_lighting.glsl";
constexpr const char* kAdapterForward = "Adapters/adapter_forward.glsl";

} // namespace

// The analytic half of the transmission energy split. Its absence is invisible to every
// keyword-derivation test: the lobe compiles, the parameters arrive, and the surface still
// renders as opaque diffuse.
TEST(GlassTransmissionShaderContract, DirectDiffuseIsScaledByTransmissionWeight)
{
    const std::string source = LoadShaderWithoutComments(kStandardPbr);
    ASSERT_FALSE(source.empty()) << "standard_pbr.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::string body = ExtractFunctionBody(source, "vec3 GE_EvaluateStandardPBR");
    ASSERT_FALSE(body.empty());
    const std::string packed = StripWhitespace(body);
    EXPECT_NE(packed.find("diffuse*=1.0-clamp(so.transmissionWeight,0.0,1.0);"), std::string::npos)
        << "GE_EvaluateStandardPBR must take the transmitted fraction out of the analytic "
           "diffuse, matching ibl.glsl's ambient diffuseScale";
}

// Gated on the keyword so a material without the lobe is byte-identical: uParams15.w is not
// guaranteed zero for a material that never opted in.
TEST(GlassTransmissionShaderContract, DirectDiffuseSplitIsGatedOnTheTransmissionKeyword)
{
    const std::string source = LoadShaderWithoutComments(kStandardPbr);
    ASSERT_FALSE(source.empty());

    const std::size_t use = source.find("diffuse *= 1.0 - clamp(so.transmissionWeight");
    ASSERT_NE(use, std::string::npos);
    const std::size_t guard = source.rfind("#ifdef GE_TRANSMISSION_ENABLED", use);
    ASSERT_NE(guard, std::string::npos)
        << "the direct diffuse split must sit under #ifdef GE_TRANSMISSION_ENABLED";
    EXPECT_EQ(source.find("#endif", use), source.find("#endif", guard))
        << "no other preprocessor block may close between the guard and the split";
}

// The ambient half, pinned alongside it: the two must move together or the split is silently
// asymmetric between the analytic and image-based lighting paths.
TEST(GlassTransmissionShaderContract, AmbientDiffuseIsScaledByTransmissionWeight)
{
    const std::string source = LoadShaderWithoutComments(kIbl);
    ASSERT_FALSE(source.empty()) << "ibl.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::string packed = StripWhitespace(source);
    EXPECT_NE(packed.find("diffuseScale=1.0-wT;"), std::string::npos)
        << "ibl.glsl must take the transmitted fraction out of the ambient diffuse";
    EXPECT_NE(packed.find("*diffuseScale"), std::string::npos)
        << "diffuseScale must actually multiply the indirect diffuse";
}

// Beer-Lambert needs a chord that VARIES over the silhouette, or the volume absorption is
// indistinguishable from a flat surface tint and the "thick core, thin edge" read it exists
// for cannot happen. The sphere proxy the thick path already assumes gives it exactly:
// 2 * radius * |cos(refracted ray, normal)|.
TEST(GlassTransmissionShaderContract, BeerLambertPathLengthUsesTheSphereChord)
{
    const std::string source = LoadShaderWithoutComments(kIbl);
    ASSERT_FALSE(source.empty());

    const std::string packed = StripWhitespace(source);
    EXPECT_NE(packed.find("floatpathLen=2.0*thick*abs(dot(T1,Nv));"), std::string::npos)
        << "the absorption path length must be the sphere chord, not a flat 2 * thick";
}

// A refractive surface is the lens, not the receiver.
TEST(GlassTransmissionShaderContract, RefractiveSurfacesReceiveNoGlassCaustic)
{
    const std::string source = LoadShaderWithoutComments(kCaustics);
    ASSERT_FALSE(source.empty()) << "caustics.glsl not found via GE_RENDERER_REPO_ROOT";

    const std::string body = ExtractFunctionBody(source, "vec3 GE_GlassCaustic");
    ASSERT_FALSE(body.empty());
    const std::size_t guard = body.find("#ifdef GE_TRANSMISSION_ENABLED");
    ASSERT_NE(guard, std::string::npos)
        << "GE_GlassCaustic must opt transmissive surfaces out of receiving the dapple";
    const std::size_t bail = body.find("return vec3(0.0);", guard);
    ASSERT_NE(bail, std::string::npos);
    EXPECT_LT(bail, body.find("#else", guard))
        << "the transmissive branch must return before any web is evaluated";
}

// The dapple is extra irradiance on a receiver, so it is returned through that receiver's
// Lambert response. Skipping the BRDF makes it a multiple of the light's full intensity: on a
// sun-lit surface a filament then outruns the surface's own direct diffuse by ~50x and clips
// to white, and a black floor gets the same white web as a white wall.
TEST(GlassTransmissionShaderContract, GlassCausticGoesThroughTheReceiverDiffuseAlbedo)
{
    const std::string source = LoadShaderWithoutComments(kCaustics);
    ASSERT_FALSE(source.empty());

    const std::string body = ExtractFunctionBody(source, "vec3 GE_GlassCaustic");
    ASSERT_FALSE(body.empty());
    const std::string packed = StripWhitespace(body);
    EXPECT_NE(packed.find("*diffuseAlbedo*kGlassCausticInvPi"), std::string::npos)
        << "the caustic must be weighted by the receiver's diffuse albedo over pi";
}

// The focused dapple is disabled, and disabling it must not have taken the translucent-shadow
// tint with it — the two share one transmittance sample (RGB = tint, A = focus), and the tint is
// a shipped feature while the dapple is a filed defect. This pins the code PATH, not a visible
// colour: the tint carries transmissionColor only, so a glass tinted by attenuationColor alone
// still casts a colourless shadow (adapter_forward.glsl, GE_GLASS_SHADOW_COLOR). Re-enabling is a
// deliberate act: turning the strength back up means deleting this test, which is the prompt to
// read why it was turned off first.
TEST(GlassTransmissionShaderContract, GlassCausticIsDisabledWhileTheShadowTintStaysOn)
{
    const std::string caustics = LoadShaderWithoutComments(kCaustics);
    ASSERT_FALSE(caustics.empty());

    const std::size_t decl = caustics.find("const float kGlassCausticStrength =");
    ASSERT_NE(decl, std::string::npos);
    EXPECT_FLOAT_EQ(std::stof(caustics.substr(caustics.find('=', decl) + 1, 16)), 0.0f)
        << "the focus term (GE_GlassFocus) is the dapple's only geometric bound and it "
           "does not bound amplitude, so the additive focused web stays off";

    const std::string shadows = LoadShaderWithoutComments("Includes/shadow_sampling.glsl");
    ASSERT_FALSE(shadows.empty());
    const std::string packedShadows = StripWhitespace(shadows);
    EXPECT_NE(packedShadows.find("ge_lastShadowTint=mix(tintSample.rgb,vec3(1.0),distanceFade);"),
              std::string::npos)
        << "the translucent-shadow tint must fade to neutral at the authored shadow distance "
           "while the caustic dapple is off";
}

// Every call site feeds it the receiver's diffuse albedo — all occurrences per file, so a
// second call site added later is held to the same contract. A metal reflects rather than
// scatters, so its albedo is the specular F0 and contributes no diffuse dapple.
TEST(GlassTransmissionShaderContract, EveryGlassCausticCallSitePassesTheReceiverAlbedo)
{
    for (const char* file : {kClusteredLighting, kAdapterForward})
    {
        const std::string source = LoadShaderWithoutComments(file);
        ASSERT_FALSE(source.empty()) << file << " not found via GE_RENDERER_REPO_ROOT";

        int callSites = 0;
        for (std::size_t call = source.find("GE_GlassCaustic("); call != std::string::npos;
             call = source.find("GE_GlassCaustic(", call + 1))
        {
            ++callSites;
            const std::size_t close = source.find(");", call);
            ASSERT_NE(close, std::string::npos) << file << " call site " << callSites;
            const std::string args = StripWhitespace(source.substr(call, close - call));
            EXPECT_NE(args.find("so.baseColor*(1.0-so.metallic)"), std::string::npos)
                << file << " call site " << callSites
                << " must pass the receiver's diffuse albedo to GE_GlassCaustic";
        }
        ASSERT_GE(callSites, 1) << file << " must call GE_GlassCaustic";
    }
}
