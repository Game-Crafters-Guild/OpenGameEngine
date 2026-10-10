#include "UI/UIManager.h"
#include "UIManager_Internal.h"

#include "Logger/Logger.h"
#include "Platform/SystemFonts.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/UiDispatcher.h"

#include "Core/Application.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Rendering::Text;

void ConfigureUiFontAtlas(FontAtlas& /*atlas*/)
{
    // No configuration needed — Slug works at any size with no precomputation parameters.
}

std::chrono::milliseconds ComputeFontRequestBackoff(uint32_t failCount)
{
    const uint32_t capped = std::min<uint32_t>(failCount, 8u);
    uint32_t ms = 250u * (1u << (capped > 0u ? (capped - 1u) : 0u));
    if (ms > 30000u)
        ms = 30000u;
    return std::chrono::milliseconds(ms);
}

// A font-retry restyle re-cascades the WHOLE tree (MarkStyleDirtyAll) so text
// nodes re-request their family. A family that never resolves (not shipped and
// not installed on this OS — e.g. "Menlo" / "DejaVu Sans Mono" on Windows)
// would otherwise reschedule that whole-tree restyle forever on exponential
// backoff, defeating the idle gate with a periodic ~9ms full rebuild. Bound the
// blind retries: after this many consecutive failures, stop scheduling
// restyles. A font that genuinely becomes available later re-resolves via the
// asset-reload path (HandleFontAssetReloaded clears the fail bookkeeping), and
// any organic restyle past the retry-after gate still re-requests it once.
constexpr uint32_t kMaxFontRetryRestyles = 3u;

void UIManager::ScheduleFontRetryRestyle(std::chrono::milliseconds backoff)
{
    // Without a scheduler host, retries ride the next organic restyle.
    if (!m_Scheduler)
        return;
    // The scheduler is owned by this manager, so pending tasks die with it —
    // capturing `this` is safe. MarkStyleDirtyAll re-cascades text nodes,
    // whose font resolution re-requests any family past its backoff.
    m_Scheduler->ScheduleAfter(backoff, [this]()
    {
        if (m_Root)
            MarkStyleDirtyAll();
    });
}

std::string UIManager::NormalizeFontFamilyKey(const std::string& family)
{
    auto isSpace = [](unsigned char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    };

    size_t b = 0;
    size_t e = family.size();
    while (b < e && isSpace((unsigned char)family[b]))
        ++b;
    while (e > b && isSpace((unsigned char)family[e - 1]))
        --e;
    if (e <= b)
        return {};

    if ((e - b) >= 2)
    {
        const char c0 = family[b];
        const char c1 = family[e - 1];
        if ((c0 == '"' && c1 == '"') || (c0 == '\'' && c1 == '\''))
        {
            ++b;
            --e;
            while (b < e && isSpace((unsigned char)family[b]))
                ++b;
            while (e > b && isSpace((unsigned char)family[e - 1]))
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
        if (isSpace(c))
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

void UIManager::OnFontResolved(const std::string& fontKey, FontResolveResult&& result, uint64_t resolverGeneration)
{
    m_FontFamilyInFlight.erase(fontKey);

    if (resolverGeneration != m_FontResolverGeneration)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (result.Bytes.empty())
    {
        const uint32_t fails = ++m_FontFamilyFailCount[fontKey];
        const auto backoff = ComputeFontRequestBackoff(fails);
        m_FontFamilyRetryAfter[fontKey] = now + backoff;
        if (fails <= kMaxFontRetryRestyles)
            ScheduleFontRetryRestyle(backoff);
        else if (fails == kMaxFontRetryRestyles + 1u)
            Logger::Log::Warning(
                "[UI Font] family key '{}' unresolved after {} retries; leaving text on fallback. "
                "No further whole-tree restyles will be scheduled for it (check CSS font-family / install the font).",
                fontKey, kMaxFontRetryRestyles);
        return;
    }

    if (m_FontAtlases.find(fontKey) != m_FontAtlases.end())
    {
        m_FontFamilyFailCount.erase(fontKey);
        m_FontFamilyRetryAfter.erase(fontKey);
        return;
    }
    if (m_FontAtlasAliases.find(fontKey) != m_FontAtlasAliases.end())
    {
        m_FontFamilyFailCount.erase(fontKey);
        m_FontFamilyRetryAfter.erase(fontKey);
        return;
    }

    auto fa = std::make_unique<FontAtlas>();
    ConfigureUiFontAtlas(*fa);
    if (!fa->LoadFontBytes(result.Bytes.data(), result.Bytes.size(), kUiFontAtlasPx))
    {
        const uint32_t fails = ++m_FontFamilyFailCount[fontKey];
        const auto backoff = ComputeFontRequestBackoff(fails);
        m_FontFamilyRetryAfter[fontKey] = now + backoff;
        if (fails <= kMaxFontRetryRestyles)
            ScheduleFontRetryRestyle(backoff);
        else if (fails == kMaxFontRetryRestyles + 1u)
            Logger::Log::Warning(
                "[UI Font] family key '{}' loaded but failed to parse after {} retries; leaving text on fallback. "
                "No further whole-tree restyles will be scheduled for it.",
                fontKey, kMaxFontRetryRestyles);
        return;
    }

    m_FontFamilyFailCount.erase(fontKey);
    m_FontFamilyRetryAfter.erase(fontKey);

    FontAtlas* faPtr = fa.get();

    Rendering::Text::FontAtlas* alias = nullptr;
    std::string aliasOwningKey; // Empty if alias is the default m_FontAtlas or no alias.
    bool aliasIsDefaultAtlas = false;
    const uint32_t newAtlasId = fa->GetAtlasId();
    if (newAtlasId != 0)
    {
        if (m_FontAtlas && m_FontAtlas->GetAtlasId() == newAtlasId)
        {
            alias = m_FontAtlas.get();
            aliasIsDefaultAtlas = true;
        }
        else
        {
            for (auto& kv : m_FontAtlases)
            {
                if (kv.second && kv.second->GetAtlasId() == newAtlasId)
                {
                    alias = kv.second.get();
                    aliasOwningKey = kv.first;
                    break;
                }
            }
        }
    }

    if (alias)
    {
        m_FontAtlasAliases[fontKey] = alias;
        faPtr = alias;
    }
    else
    {
        m_FontAtlases[fontKey] = std::move(fa);
    }

    // Track (font GUID -> atlas keys) so AssetReloaded(Font) events can evict
    // the matching atlas instance. Resolvers that don't identify a GUID
    // (system fonts, etc.) leave assetGuid null and we skip the mapping —
    // those fonts simply don't participate in hot reload.
    //
    // When the new request resolves to an existing atlas via aliasing, we
    // also record the *owning* key in the GUID's key list so the reload
    // handler erases the actual m_FontAtlases entry, not just the alias
    // entry which would leave the underlying atlas alive with stale bytes.
    // The default m_FontAtlas (no key) is tracked via m_DefaultFontAtlasGuid.
    if (!result.AssetGuid.IsNull())
    {
        m_AtlasKeyToFontGuid[fontKey] = result.AssetGuid;
        auto& keys = m_FontGuidToAtlasKeys[result.AssetGuid];
        if (std::find(keys.begin(), keys.end(), fontKey) == keys.end())
            keys.push_back(fontKey);
        if (!aliasOwningKey.empty() && aliasOwningKey != fontKey)
        {
            if (std::find(keys.begin(), keys.end(), aliasOwningKey) == keys.end())
                keys.push_back(aliasOwningKey);
        }
        if (aliasIsDefaultAtlas)
            m_DefaultFontAtlasGuid = result.AssetGuid;
    }

    static const bool s_LogFonts = []()
    {
        if (const char* e = std::getenv("GE_UI_FONT_DEBUG"))
            return (e && e[0] == '1');
        return false;
    }();
    if (s_LogFonts && faPtr)
    {
        Logger::Log::Info("[UI Font] Installed key='{}' bytes={} debug='{}'",
                          fontKey,
                          result.Bytes.size(),
                          result.DebugName.empty() ? "<none>" : result.DebugName.c_str());
        if (auto info = faPtr->GetFaceDebugInfo())
        {
            Logger::Log::Info("[UI Font] Face family='{}' style='{}' ps='{}' glyphs={} em={} asc={} desc={} height={}",
                              info->family.empty() ? "<none>" : info->family.c_str(),
                              info->style.empty() ? "<none>" : info->style.c_str(),
                              info->postscript.empty() ? "<none>" : info->postscript.c_str(),
                              info->numGlyphs,
                              info->unitsPerEm,
                              info->ascender,
                              info->descender,
                              info->height);
        }
    }

    m_FontResolvedDirty = true;
}

bool UIManager::SetDefaultFontBytes(const std::vector<std::uint8_t>& bytes, GUID assetGuid)
{
    if (bytes.empty())
        return false;

    auto fa = std::make_shared<FontAtlas>();
    ConfigureUiFontAtlas(*fa);
    if (!fa->LoadFontBytes(bytes.data(), bytes.size(), kUiFontAtlasPx))
        return false;

    m_FontAtlas = std::move(fa);
    m_DefaultFontAtlasGuid = assetGuid;
    m_FontResolvedDirty = true;
    return true;
}

FontAtlas* UIManager::GetOrRequestFontFamilyInternal(const std::string& family,
                                                     int weight,
                                                     FontStyle style,
                                                     FontVariant variant)
{
    auto clampWeight = [](int w) -> int
    {
        if (w < 1)
            w = 1;
        if (w > 1000)
            w = 1000;
        return w;
    };

    const std::string famKey = NormalizeFontFamilyKey(family);
    if (famKey.empty())
        return nullptr;

    const std::string key = famKey + "|" + std::to_string(clampWeight(weight)) + "|" + std::to_string((int)style) + "|" +
                            std::to_string((int)variant);

    auto itAlias = m_FontAtlasAliases.find(key);
    if (itAlias != m_FontAtlasAliases.end())
        return itAlias->second;

    auto itLoaded = m_FontAtlases.find(key);
    if (itLoaded != m_FontAtlases.end())
        return itLoaded->second.get();

    const auto now = std::chrono::steady_clock::now();
    auto itRetry = m_FontFamilyRetryAfter.find(key);
    if (itRetry != m_FontFamilyRetryAfter.end() && now < itRetry->second)
        return nullptr;
    if (m_FontFamilyInFlight.find(key) != m_FontFamilyInFlight.end())
        return nullptr;
    // Legacy fallback: if no resolver is installed, attempt to load a best-effort family font
    // from disk. This keeps UI usable in tests/tools that do not integrate with the engine/editor
    // font pipeline.
    if (!m_FontResolver)
    {
        std::vector<uint8_t> fontBytes;
        std::string debugName;
        auto tryLoad = [&](const char* p)
        {
            if (!fontBytes.empty())
                return;
            fontBytes = Utils::ReadFile(p);
            if (!fontBytes.empty())
                debugName = p;
        };
        static const bool s_LogFonts = []()
        {
            if (const char* e = std::getenv("GE_UI_FONT_DEBUG"))
                return (e && e[0] == '1');
            return false;
        }();

        const std::string& famLower = famKey; // already lowercased

        // Pick a shipped Roboto face. Check Roboto Mono first because "roboto
        // mono" contains "roboto" and would otherwise be captured by the
        // proportional-Roboto branch below.
        //
        // Weight mapping (proportional Roboto ships Regular / Medium / Bold):
        //   [1   .. 499]  → Regular (400)
        //   [500 .. 649]  → Medium (500)  — CSS `font-weight: 500` or semibold (600)
        //   [650 ..    ]  → Bold (700)
        // This keeps "semibold" (600) from looking like full Bold, which was
        // too heavy for section headers.
        const char* shippedFace = nullptr;
        {
            const bool wantItalic = (style == FontStyle::Italic || style == FontStyle::Oblique);
            const bool wantBold   = (weight >= 650);
            const bool wantMedium = (weight >= 500 && weight < 650);
            const bool wantMono = famLower.find("roboto mono") != std::string::npos
                               || famLower.find("robotomono") != std::string::npos;
            if (wantMono)
            {
                // Roboto Mono ships only Regular/Bold + italic variants. Medium
                // requests fall back to Regular — the Mono design doesn't
                // really need an intermediate weight for code readability.
                shippedFace = (wantBold && wantItalic) ? "RobotoMono-BoldItalic.ttf"
                            : (wantBold)               ? "RobotoMono-Bold.ttf"
                            : (wantItalic)             ? "RobotoMono-Italic.ttf"
                                                       : "RobotoMono-Regular.ttf";
            }
            else if (famLower.find("roboto") != std::string::npos)
            {
                if (wantBold)
                    shippedFace = wantItalic ? "Roboto-BoldItalic.ttf" : "Roboto-Bold.ttf";
                else if (wantMedium)
                    shippedFace = wantItalic ? "Roboto-MediumItalic.ttf" : "Roboto-Medium.ttf";
                else
                    shippedFace = wantItalic ? "Roboto-Italic.ttf" : "Roboto-Regular.ttf";
            }
        }
        if (shippedFace)
        {
            const std::string stagedFace = (PathUtils::GetInstallAssetsRoot() / "Fonts" / shippedFace).string();
            tryLoad(stagedFace.c_str());
        }
        if (fontBytes.empty())
        {
            const std::string stagedFamily =
                (PathUtils::GetInstallAssetsRoot() / "Fonts" / (family + ".ttf")).string();
            tryLoad(stagedFamily.c_str());
        }
#if !defined(__APPLE__)
        if (fontBytes.empty())
        {
            auto toSysStyle = [](FontStyle s) -> Platform::SystemFontStyle {
                if (s == FontStyle::Italic)  return Platform::SystemFontStyle::Italic;
                if (s == FontStyle::Oblique) return Platform::SystemFontStyle::Oblique;
                return Platform::SystemFontStyle::Normal;
            };
            Platform::SystemFontFile sf;
            if (Platform::TryResolveSystemFontFile(family, weight, toSysStyle(style), sf))
                tryLoad(sf.path.string().c_str());
        }
#endif

        if (fontBytes.empty())
        {
            const uint32_t fails = ++m_FontFamilyFailCount[key];
            m_FontFamilyRetryAfter[key] = now + ComputeFontRequestBackoff(fails);
            return nullptr;
        }

        auto fa = std::make_unique<FontAtlas>();
        ConfigureUiFontAtlas(*fa);
        if (!fa->LoadFontBytes(fontBytes.data(), fontBytes.size(), kUiFontAtlasPx))
        {
            const uint32_t fails = ++m_FontFamilyFailCount[key];
            m_FontFamilyRetryAfter[key] = now + ComputeFontRequestBackoff(fails);
            return nullptr;
        }

        m_FontFamilyFailCount.erase(key);
        m_FontFamilyRetryAfter.erase(key);

        if (s_LogFonts)
        {
            Logger::Log::Info("[UI Font] Installed key='{}' bytes={} debug='{}'",
                              key,
                              fontBytes.size(),
                              debugName.empty() ? "<none>" : debugName.c_str());
            if (auto info = fa->GetFaceDebugInfo())
            {
                Logger::Log::Info("[UI Font] Face family='{}' style='{}' ps='{}' glyphs={} em={} asc={} desc={} height={}",
                                  info->family.empty() ? "<none>" : info->family.c_str(),
                                  info->style.empty() ? "<none>" : info->style.c_str(),
                                  info->postscript.empty() ? "<none>" : info->postscript.c_str(),
                                  info->numGlyphs,
                                  info->unitsPerEm,
                                  info->ascender,
                                  info->descender,
                                  info->height);
            }
        }

        auto* ret = fa.get();
        m_FontAtlases[key] = std::move(fa);
        return ret;
    }

    // Host resolver path (async-capable).
    m_FontFamilyInFlight.insert(key);

    // Shared, so a resolver that completes on another thread after this manager is gone posts
    // into a closed dispatcher, which refuses it. Posted work runs only from this manager's drains.
    std::shared_ptr<UI::UiDispatcher> dispatcher = m_Dispatcher;
    FontResolverFn resolver = m_FontResolver;
    const uint64_t gen = m_FontResolverGeneration;
    const std::string familyCopy = family;
    const int weightCopy = clampWeight(weight);
    const FontStyle styleCopy = style;
    const FontVariant variantCopy = variant;

    auto complete = [this, key, gen, dispatcher](FontResolveResult res) mutable
    {
        dispatcher->Post([this, key, gen, res = std::move(res)]() mutable
                         {
                             if (!m_AppliesAsyncResults)
                                 return;
                             OnFontResolved(key, std::move(res), gen); });
    };

    try
    {
        resolver(familyCopy, weightCopy, styleCopy, variantCopy, std::move(complete));
    }
    catch (...)
    {
        complete(FontResolveResult{});
    }

    return nullptr;
}
