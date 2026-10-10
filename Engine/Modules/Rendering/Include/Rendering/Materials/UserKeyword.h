#pragma once

// User shader keywords (extensibility slice N): open, author-minted keyword
// names carried per-material in the .material `keywords` array. Each is
// sanitized to a valid C identifier and emitted GE_USER_-namespaced so it can
// never clobber an engine define. This header is the single source of the
// sanitize rule + reserved set, shared by the parser, the variant-key hash, and
// the composer emission.

#include <cctype>
#include <optional>
#include <string>
#include <unordered_set>

namespace GameEngine
{
namespace Rendering
{

// A material enables at most this many keywords (§2.4). Bounds the preamble and
// forbids an accidental wide multi_compile surface; extras are truncated at parse.
inline constexpr int kMaxUserKeywords = 8;

// Unprefixed engine defines a sanitized keyword must never become. GE_USER_
// namespacing already makes real collision impossible; this is belt-and-braces
// so an author writing `ALPHA_TEST` as a keyword is rejected loudly rather than
// shadowed. Mirrors the emissions in ShaderVariantKey.h GenerateDefines plus the
// path-based surface defines in ShaderComposer.
inline const std::unordered_set<std::string>& ReservedEngineDefines()
{
    static const std::unordered_set<std::string> kReserved = {
        "HAS_POSITION", "HAS_NORMAL", "HAS_UV0", "HAS_TANGENT", "HAS_UV1", "HAS_UV2",
        "HAS_UV3", "HAS_UV4", "HAS_UV5", "HAS_UV6", "HAS_UV7", "HAS_COLOR", "SKINNED",
        "SKINNED_8", "ALPHA_TEST", "GE_ALPHA_BLEND", "GE_INSTANCED", "HAS_VERTEX_MODIFIER",
        "HAS_VERTEX_OUTPUT_MODIFIER", "GE_PROCEDURAL_VERTEX_OUTPUT", "FORWARD_PLUS",
        "HAS_SHADOWS", "GE_IBL_ENABLED", "GE_CLEARCOAT_ENABLED", "GE_SHEEN_ENABLED",
        "GE_ANISOTROPY_ENABLED", "GE_SUBSURFACE_ENABLED", "GE_DEPTH_ONLY_FRAGMENT",
        "GE_TRANSMISSION_ENABLED", "GE_SCENECOLOR_GRAB", "GE_TRANSMISSION_THICK",
        "GE_GLASS_SHADOW_COLOR", "GE_GTAO_ENABLED", "GE_RT_SHADOW_MASK_ENABLED", "GE_SCREEN_SPACE_SHADOWS_ENABLED",
        "GE_IRIDESCENCE_ENABLED",
        "GE_FUZZ_ENABLED", "GE_COAT_NORMAL_ENABLED", "CUSTOM_VERTEX_SHADER",
        "GE_TWO_SIDED_KEEP_NORMAL", "GE_LOD_CROSSFADE",
        "GE_VERTEX_DEFORMATION", "GE_MOTION_VECTORS",
        // Names whose GE_USER_-prefixed form collides with a function-like
        // engine macro in the adapters (GE_USER_TEXTURE(name) — slice K's
        // named-slot accessor). The namespace alone can't catch these.
        "TEXTURE"};
    return kReserved;
}

// Sanitize a raw authored keyword to a valid, uppercase C identifier. Returns
// nullopt when it is empty, contains a non-identifier character, starts with a
// digit, or collides with a reserved engine define — those are rejected/skipped,
// never emitted (the preamble is raw textual injection). The returned form is
// idempotent (sanitize(sanitize(x)) == sanitize(x)), so storing it round-trips.
inline std::optional<std::string> SanitizeUserKeyword(const std::string& raw)
{
    if (raw.empty())
        return std::nullopt;
    std::string out;
    out.reserve(raw.size());
    for (char c : raw)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_')
            out.push_back(static_cast<char>(std::toupper(uc)));
        else
            return std::nullopt; // invalid character -> reject the whole keyword
    }
    if (std::isdigit(static_cast<unsigned char>(out[0])))
        return std::nullopt; // an identifier cannot start with a digit
    if (ReservedEngineDefines().count(out) != 0)
        return std::nullopt;
    return out;
}

} // namespace Rendering
} // namespace GameEngine
