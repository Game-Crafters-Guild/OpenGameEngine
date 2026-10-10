#include "Engine/UI/FontResolver.h"

#include "Assets/AssetRegistry.h"
#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Platform/SystemFonts.h"
#include "Rendering/Common/Utils.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

namespace GameEngine::Engine::UI
{
namespace
{
static bool IsSpace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static std::string Trim(std::string_view s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsSpace((unsigned char)s[b]))
        ++b;
    while (e > b && IsSpace((unsigned char)s[e - 1]))
        --e;
    if (e <= b)
        return {};
    return std::string(s.substr(b, e - b));
}

static std::string ToLowerAscii(std::string s)
{
    for (auto& ch : s)
        ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static int ClampCssWeight(int w)
{
    if (w < 1)
        w = 1;
    if (w > 1000)
        w = 1000;
    return w;
}

static std::string InsertSpacesBeforeCaps(const std::string& s)
{
    if (s.empty())
        return s;
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back(s[0]);
    for (size_t i = 1; i < s.size(); ++i)
    {
        const unsigned char c = (unsigned char)s[i];
        const unsigned char prev = (unsigned char)s[i - 1];
        const bool isUpper = std::isupper(c) != 0;
        const bool prevLower = std::islower(prev) != 0;
        const bool prevDigit = std::isdigit(prev) != 0;
        if (isUpper && (prevLower || prevDigit))
            out.push_back(' ');
        out.push_back((char)c);
    }
    return out;
}

static std::string DeriveFamilyFromFilename(const std::filesystem::path& p)
{
    std::string stem = p.stem().string();
    if (stem.empty())
        return {};

    // Normalize common separators to '-'.
    for (auto& ch : stem)
    {
        if (ch == '_')
            ch = '-';
    }

    // Common convention: Family-Style (e.g. Roboto-Regular.ttf)
    const size_t dash = stem.find('-');
    if (dash != std::string::npos && dash > 0)
        stem = stem.substr(0, dash);

    return Trim(stem);
}

static std::filesystem::path ToAbsolutePath(const std::filesystem::path& p, const std::filesystem::path& assetRoot)
{
    if (p.empty())
        return {};
    if (p.is_absolute())
        return p;
    if (assetRoot.empty())
        return p;
    return (assetRoot / p).lexically_normal();
}

} // namespace

FontFamilyIndex::FontFamilyIndex(AssetRegistry& registry)
    : m_Registry(registry)
{
}

std::string FontFamilyIndex::NormalizeKey(const std::string& family)
{
    size_t b = 0;
    size_t e = family.size();
    while (b < e && IsSpace((unsigned char)family[b]))
        ++b;
    while (e > b && IsSpace((unsigned char)family[e - 1]))
        --e;
    if (e <= b)
        return {};

    // Strip surrounding quotes (best-effort).
    if ((e - b) >= 2)
    {
        const char c0 = family[b];
        const char c1 = family[e - 1];
        if ((c0 == '"' && c1 == '"') || (c0 == '\'' && c1 == '\''))
        {
            ++b;
            --e;
            while (b < e && IsSpace((unsigned char)family[b]))
                ++b;
            while (e > b && IsSpace((unsigned char)family[e - 1]))
                --e;
            if (e <= b)
                return {};
        }
    }

    std::string out;
    out.reserve(e - b);
    bool prevSpace = false;
    for (size_t i = b; i < e; ++i)
    {
        const unsigned char c = (unsigned char)family[i];
        if (IsSpace(c))
        {
            if (!out.empty() && !prevSpace)
            {
                out.push_back(' ');
                prevSpace = true;
            }
            continue;
        }
        out.push_back((char)std::tolower(c));
        prevSpace = false;
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

void FontFamilyIndex::DeriveCandidateTraitsFromFilename(const std::filesystem::path& p, int& outWeight, FontStyle& outStyle)
{
    const std::string stemLower = ToLowerAscii(p.stem().string());
    outWeight = 400;
    outStyle = FontStyle::Normal;

    auto has = [&](const char* needle) -> bool
    { return stemLower.find(needle) != std::string::npos; };

    if (has("italic"))
        outStyle = FontStyle::Italic;
    else if (has("oblique"))
        outStyle = FontStyle::Oblique;

    if (has("thin"))
        outWeight = 100;
    else if (has("extralight") || has("ultralight"))
        outWeight = 200;
    else if (has("light"))
        outWeight = 300;
    else if (has("regular") || has("roman") || has("book") || has("normal"))
        outWeight = 400;
    else if (has("medium"))
        outWeight = 500;
    else if (has("semibold") || has("demibold"))
        outWeight = 600;
    else if (has("bold"))
        outWeight = 700;
    else if (has("extrabold") || has("ultrabold"))
        outWeight = 800;
    else if (has("black") || has("heavy"))
        outWeight = 900;
}

void FontFamilyIndex::Rebuild()
{
    m_CandidatesByKey.clear();

    const std::filesystem::path assetRoot = m_Registry.GetAssetRoot();
    const Vector<GUID> fonts = m_Registry.GetAssetsByType(AssetType::Font);
    m_SourceFontCount = fonts.size();
    m_CandidatesByKey.reserve(fonts.size());

    for (const GUID& guid : fonts)
    {
        AssetMetadata md{};
        if (!m_Registry.TryGetAssetMetadata(guid, md))
            continue;

        const std::filesystem::path absPath = ToAbsolutePath(md.Path, assetRoot);
        if (absPath.empty())
            continue;

        // The registry can carry registrations whose backing file is gone
        // (e.g. staged copies persisted from a deleted build tree). Indexing
        // one poisons its family: the weight matcher happily picks it and the
        // byte read then fails every retry. Fonts are few and Rebuild is
        // rare, so an existence probe here is cheap insurance.
        std::error_code existsEc;
        if (!std::filesystem::exists(absPath, existsEc))
            continue;

        // Family names and traits come from the filename.
        std::vector<std::string> families;
        int candWeight = 400;
        FontStyle candStyle = FontStyle::Normal;
        {
            std::string fam = DeriveFamilyFromFilename(absPath);
            if (!fam.empty())
            {
                families.push_back(fam);
                const std::string spaced = InsertSpacesBeforeCaps(fam);
                if (spaced != fam)
                    families.push_back(spaced);
            }
        }

        // Always include the raw stem as a last-resort alias (e.g. "Roboto-Regular").
        {
            const std::string stem = absPath.stem().string();
            if (!stem.empty())
                families.push_back(stem);
        }

        // Deduplicate family strings (case-insensitive via normalized key).
        std::unordered_set<std::string> seenKeys;
        seenKeys.reserve(families.size());

        DeriveCandidateTraitsFromFilename(absPath, candWeight, candStyle);

        for (const std::string& fam : families)
        {
            const std::string key = NormalizeKey(fam);
            if (key.empty())
                continue;
            if (!seenKeys.insert(key).second)
                continue;

            Candidate c{};
            c.guid = guid;
            c.path = absPath;
            c.weight = candWeight;
            c.style = candStyle;
            c.debugName = absPath.string();

            m_CandidatesByKey[key].push_back(std::move(c));
        }
    }

    m_Built = true;
}

bool FontFamilyIndex::RebuildIfSourceChanged()
{
    if (m_Built && m_Registry.GetAssetsByType(AssetType::Font).size() == m_SourceFontCount)
        return false;
    Rebuild();
    return true;
}

bool FontFamilyIndex::TryResolveFontAssetPath(const std::string& family,
                                              int weight,
                                              FontStyle style,
                                              std::filesystem::path& outAbsPath,
                                              std::string* outDebugName,
                                              GUID* outAssetGuid) const
{
    const std::string key = NormalizeKey(family);
    if (key.empty())
        return false;
    if (!m_Built)
        return false;

    auto it = m_CandidatesByKey.find(key);
    if (it == m_CandidatesByKey.end() || it->second.empty())
        return false;

    const int reqW = ClampCssWeight(weight);
    const Candidate* best = nullptr;
    int bestScore = 1 << 30;

    for (const Candidate& c : it->second)
    {
        int score = 0;
        if (c.style != style)
        {
            // Prefer normal as the general fallback, otherwise penalize heavily.
            score += (c.style == FontStyle::Normal) ? 2000 : 5000;
        }

        // CSS Fonts Level 4, §5.2: weight matching direction depends on the
        // requested weight. Candidates in the "wrong" direction get a large
        // base penalty so the "right" direction always wins, then distance
        // breaks ties within the same direction.
        const int diff = c.weight - reqW; // positive = heavier
        if (reqW > 500)
        {
            // Prefer heavier first, then lighter.
            score += (diff >= 0) ? diff : (1000 + (-diff));
        }
        else if (reqW >= 400)
        {
            // Prefer [requested..500] first, then lighter, then heavier.
            if (diff >= 0 && c.weight <= 500)
                score += diff;
            else if (diff < 0)
                score += 500 + (-diff);
            else
                score += 1000 + diff;
        }
        else
        {
            // Prefer lighter first, then heavier.
            score += (diff <= 0) ? (-diff) : (1000 + diff);
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = &c;
        }
    }

    if (!best)
        return false;

    outAbsPath = best->path;
    if (outDebugName)
        *outDebugName = best->debugName;
    if (outAssetGuid)
        *outAssetGuid = best->guid;
    return !outAbsPath.empty();
}

FontResolver::FontResolver(AssetRegistry& registry, JobSystem::WorkStealingThreadPool* jobSystem)
    : m_Index(registry), m_JobSystem(jobSystem)
{
}

void FontResolver::RebuildIndex()
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Index.Rebuild();
    m_NegativeUntil.clear();
    // NOTE: std::once_flag cannot be reset; callers that need rebuild semantics should call
    // RebuildIndex() explicitly before any ResolveAsync() calls (or accept eventual consistency).
}

namespace
{
// Shared by ResolveAsync and TryIsFamilyResolvable: locate a font file without reading bytes.
static bool TryLocateFamilyFontFile(FontFamilyIndex& index,
                                    bool allowSystemFonts,
                                    const std::string& family,
                                    int weight,
                                    FontStyle style,
                                    std::filesystem::path& outAbsPath,
                                    uint32_t& outFaceIndex,
                                    std::string* outDebugName,
                                    GUID* outAssetGuid = nullptr)
{
    bool fromAssets = index.TryResolveFontAssetPath(family, weight, style, outAbsPath, outDebugName, outAssetGuid);
    outFaceIndex = 0;
    if (outAssetGuid && !fromAssets)
        *outAssetGuid = GUID::Null();
    if (fromAssets)
        return true;
    if (!allowSystemFonts)
        return false;

    const std::string famKey = FontFamilyIndex::NormalizeKey(family);

    Platform::SystemFontFile sf{};
    Platform::SystemFontStyle sysStyle = Platform::SystemFontStyle::Normal;
    if (style == FontStyle::Italic)
        sysStyle = Platform::SystemFontStyle::Italic;
    else if (style == FontStyle::Oblique)
        sysStyle = Platform::SystemFontStyle::Oblique;

    auto buildCandidates = [](const std::string& s, const std::string& normalized) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        out.reserve(4);
        if (!s.empty())
            out.push_back(s);

        std::string spaced = s;
        for (char& c : spaced)
        {
            if (c == '-' || c == '_')
                c = ' ';
        }
        if (spaced != s)
            out.push_back(std::move(spaced));

#ifdef _WIN32
        if (normalized == "comic sans" || normalized == "comic-sans" || normalized == "comicsans")
            out.push_back("Comic Sans MS");
#else
        (void)normalized;
#endif
        return out;
    };

    // CSS generic families (sans-serif, serif, monospace, system-ui) are
    // mapped to a concrete OS font by Platform::TryResolveSystemFontFile.
    const auto candidates = buildCandidates(family, famKey);
    for (const auto& c : candidates)
    {
        if (Platform::TryResolveSystemFontFile(c, ClampCssWeight(weight), sysStyle, sf))
        {
            outAbsPath = sf.path;
            outFaceIndex = sf.faceIndex;
            if (outDebugName)
                *outDebugName = sf.path.string();
            return true;
        }
    }
    return false;
}
} // namespace

bool FontResolver::TryIsFamilyResolvable(const std::string& family, int weight, FontStyle style)
{
    std::call_once(m_BuildOnce, [this]() { m_Index.Rebuild(); });
    if (FontFamilyIndex::NormalizeKey(family).empty())
        return false;
    std::filesystem::path absPath;
    uint32_t faceIndex = 0;
    std::string dbg;
    return TryLocateFamilyFontFile(m_Index, m_AllowSystemFonts, family, weight, style, absPath, faceIndex, &dbg);
}

void FontResolver::ResolveAsync(const std::string& family,
                                int weight,
                                FontStyle style,
                                FontVariant /*variant*/,
                                FontResolveCallback onReady)
{
    if (!onReady)
        return;

    // Ensure the index is available at first use.
    std::call_once(m_BuildOnce, [this]() { m_Index.Rebuild(); });

    const std::string famKey = FontFamilyIndex::NormalizeKey(family);
    const std::string requestKey = famKey + "|" + std::to_string(ClampCssWeight(weight)) + "|" + std::to_string((int)style);
    if (requestKey.empty())
    {
        onReady(FontBytes{});
        return;
    }

    // Fast negative cache on the requested (family, weight, style). This avoids repeated expensive
    // system font queries when a family genuinely doesn't exist.
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        const auto now = std::chrono::steady_clock::now();
        auto itNeg = m_NegativeUntil.find(requestKey);
        if (itNeg != m_NegativeUntil.end() && now < itNeg->second)
        {
            onReady(FontBytes{});
            return;
        }
    }

    // Resolve font file path (assets first, then OS system fonts).
    std::filesystem::path absPath;
    std::string debugName;
    uint32_t faceIndex = 0;
    GUID resolvedGuid = GUID::Null();
    bool fromAssets = TryLocateFamilyFontFile(m_Index, m_AllowSystemFonts, family, weight, style, absPath, faceIndex, &debugName, &resolvedGuid);

    // On a full miss, the index snapshot may simply predate the font: the
    // first resolve often races the async asset scan (an empty snapshot made
    // every shipped face unresolvable forever), and project fonts register
    // long after startup. Refresh against the registry and retry once.
    if (!fromAssets && m_Index.RebuildIfSourceChanged())
        fromAssets = TryLocateFamilyFontFile(m_Index, m_AllowSystemFonts, family, weight, style, absPath, faceIndex, &debugName, &resolvedGuid);

    if (!fromAssets)
    {
        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            m_NegativeUntil[requestKey] = std::chrono::steady_clock::now() + m_NegativeTtl;
        }
        onReady(FontBytes{});
        return;
    }

    // IMPORTANT:
    // Many UIs request "bold" even when only a single face exists (e.g. Roboto-Regular.ttf).
    // If we key in-flight and negative caches by (family, weight, style), we can end up loading
    // the same font bytes under multiple cache keys, which can lead to unstable glyph output in
    // downstream caches. Instead, key by the resolved font file + face index.
    const std::string resolvedKey = absPath.string() + "|" + std::to_string(faceIndex);

    // Dedupe + negative cache by resolved file.
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        const auto now = std::chrono::steady_clock::now();
        auto itNeg = m_NegativeUntil.find(resolvedKey);
        if (itNeg != m_NegativeUntil.end() && now < itNeg->second)
        {
            // Also dampen the original request key so we don't repeatedly resolve.
            m_NegativeUntil[requestKey] = std::chrono::steady_clock::now() + m_NegativeTtl;
            onReady(FontBytes{});
            return;
        }

        auto it = m_InFlight.find(resolvedKey);
        if (it != m_InFlight.end())
        {
            it->second.push_back(std::move(onReady));
            return;
        }
        m_InFlight[resolvedKey].push_back(std::move(onReady));
    }

    auto finish = [this, resolvedKey, requestKey](FontBytes res) mutable
    {
        std::vector<FontResolveCallback> callbacks;
        {
            std::lock_guard<std::mutex> lk(m_Mutex);
            auto it = m_InFlight.find(resolvedKey);
            if (it != m_InFlight.end())
            {
                callbacks = std::move(it->second);
                m_InFlight.erase(it);
            }
            if (res.bytes.empty())
            {
                m_NegativeUntil[resolvedKey] = std::chrono::steady_clock::now() + m_NegativeTtl;
                m_NegativeUntil[requestKey] = std::chrono::steady_clock::now() + m_NegativeTtl;
            }
            else
            {
                m_NegativeUntil.erase(resolvedKey);
                m_NegativeUntil.erase(requestKey);
            }
        }

        for (size_t i = 0; i < callbacks.size(); ++i)
        {
            if (!callbacks[i])
                continue;
            if (i + 1 == callbacks.size())
            {
                callbacks[i](std::move(res));
            }
            else
            {
                callbacks[i](res);
            }
        }
    };

    // Read bytes (async if we have a job system)
    if (m_JobSystem)
    {
        const std::string absPathStr = absPath.string();
        const std::string dbg = debugName;
        const uint32_t idx = faceIndex;
        const GUID guidCopy = resolvedGuid;
        JobSystem::TaskHandle h = m_JobSystem->Submit([absPathStr, dbg, idx, guidCopy]() -> FontBytes
                                                     {
                                                         FontBytes out{};
                                                         out.debugName = dbg.empty() ? absPathStr : dbg;
                                                         out.faceIndex = (int)idx;
                                                         out.assetGuid = guidCopy;
                                                         out.bytes = Rendering::Utils::ReadFile(absPathStr.c_str());
                                                         return out;
                                                     });
        h.OnComplete([finish](const JobSystem::TaskHandle& th) mutable
                     {
                         FontBytes res{};
                         (void)th.TryGetResult(res);
                         finish(std::move(res));
                     });
        h.OnFailure([finish, absPathStr](const JobSystem::String& err) mutable
                    {
                        Logger::Log::Warning("UI FontResolver: failed to read '{}': {}", absPathStr, err);
                        finish(FontBytes{});
                    });
        return;
    }

    // Synchronous fallback: read on the caller thread.
    FontBytes res{};
    res.debugName = debugName.empty() ? absPath.string() : debugName;
    res.faceIndex = (int)faceIndex;
    res.assetGuid = resolvedGuid;
    res.bytes = Rendering::Utils::ReadFile(absPath.string().c_str());
    finish(std::move(res));
}

} // namespace GameEngine::Engine::UI

