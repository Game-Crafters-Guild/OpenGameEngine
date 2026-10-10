#include "UI/Assets/UIStyleAsset.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/StyleUtil.h"
#include "Logger/Logger.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AssetManager.h"
#include "CssImports.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <unordered_set>

using namespace GameEngine;
using namespace GameEngine::UIParsing;

namespace
{

struct ParsedCssSegment
{
    enum class SegmentKind : uint8_t
    {
        CssText = 0,
        Import
    };

    SegmentKind Kind = SegmentKind::CssText;
    int StartLine = 1; // 1-based line number in the importing file
    std::string CssText; // for SegmentKind::CssText
    std::filesystem::path ImportResolved; // for SegmentKind::Import
};

static bool HasNonWhitespace(std::string_view text)
{
    return std::any_of(text.begin(), text.end(),
                       [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
}

static int CountNewlines(std::string_view text)
{
    return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

static void SplitCssIntoSegments(const std::filesystem::path& importerPath,
                                 const std::string& cssText,
                                 std::vector<ParsedCssSegment>& outSegments)
{
    outSegments.clear();

    const std::string_view css(cssText);
    const AssetManager* assets = AssetManager::GetThreadCurrent();
    int line = 1;
    size_t textStart = 0;

    auto pushCssText = [&](std::string_view text)
    {
        if (HasNonWhitespace(text))
        {
            ParsedCssSegment seg;
            seg.Kind = ParsedCssSegment::SegmentKind::CssText;
            seg.StartLine = line;
            seg.CssText = std::string(text);
            outSegments.push_back(std::move(seg));
        }
        line += CountNewlines(text);
    };

    for (const UI::CssImportStatement& statement : UI::FindTopLevelCssImports(css))
    {
        pushCssText(css.substr(textStart, statement.Begin - textStart));

        ParsedCssSegment seg;
        seg.Kind = ParsedCssSegment::SegmentKind::Import;
        seg.StartLine = line;
        seg.ImportResolved = UI::ResolveCssImportPath(importerPath, statement.Target, assets);
        outSegments.push_back(std::move(seg));

        line += CountNewlines(css.substr(statement.Begin, statement.End - statement.Begin));
        textStart = statement.End;
    }
    pushCssText(css.substr(textStart));
}

} // namespace

// Helper: compute 1-based line number of a byte offset
static int ComputeLineNumber(const std::string& text, size_t offset) {
    int line = 1;
    for (size_t i = 0; i < text.size() && i < offset; ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

	// Helper: naive scan for url(...) occurrences; returns mapping from raw inner text to first-seen line number
static std::unordered_map<std::string, int> BuildUrlToLineMap(const std::string& cssText) {
    std::unordered_map<std::string, int> map;
    size_t pos = 0;
    while (true) {
        pos = cssText.find("url(", pos);
        if (pos == std::string::npos) break;
        size_t open = pos + 4;
        size_t i = open;
        bool inQuote = false; char quoteChar = 0;
        // Skip whitespace
        while (i < cssText.size() && (cssText[i] == ' ' || cssText[i] == '\t' || cssText[i] == '\r' || cssText[i] == '\n')) ++i;
        if (i < cssText.size() && (cssText[i] == '\'' || cssText[i] == '"')) { inQuote = true; quoteChar = cssText[i++]; }
        size_t start = i;
        while (i < cssText.size()) {
            char c = cssText[i];
            if (inQuote) {
                if (c == quoteChar) { break; }
            } else if (c == ')') { break; }
            i++;
        }
        size_t end = i;
        std::string inner = cssText.substr(start, end - start);
        // Trim whitespace
        auto trim = [](std::string& s){
            size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
            while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
            s = s.substr(a, b - a);
        };
        trim(inner);
        if (!inner.empty() && !map.count(inner)) {
            map.emplace(inner, ComputeLineNumber(cssText, pos));
        }
        pos = (i < cssText.size()) ? i + 1 : i;
    }
    return map;
}



static void PostProcessAndWarn(Stylesheet& sheet, const std::string& cssText, const std::string& sourceName, int baseLine) {
    // Best-effort source mapping
    auto urlLineMap = BuildUrlToLineMap(cssText);

    // Fill stylesheet source name
    sheet.SourceName = sourceName;
    WarnUnknownStylesheetProperties(sheet);

    // Populate sourceLine for background-image properties (unconditionally)
    for (auto& rule : sheet.Rules) {
        for (auto& prop : rule.Properties) {
            if (prop.PropertyId != StylePropertyId::BackgroundImage)
                continue;
            const auto& src = std::get<BackgroundImageSource>(prop.Value);
            if (src.Kind != BackgroundImageSource::SourceKind::Path || src.Value.empty())
                continue;
            auto itLine = urlLineMap.find(src.Value);
            if (itLine != urlLineMap.end())
            {
                const int localLine = itLine->second;
                prop.SourceLine = (baseLine <= 1) ? localLine : (baseLine + localLine - 1);
            }
        }
    }

	    // Resolve background-image paths immediately and warn with file:line.
        // Use the AssetManager currently performing the load/reload on this thread.
        AssetManager* am = AssetManager::GetThreadCurrent();
        if (!am)
            return;
        const std::filesystem::path importerPath(sourceName);
	    for (auto& rule : sheet.Rules) {
	        for (auto& prop : rule.Properties) {
                if (prop.PropertyId != StylePropertyId::BackgroundImage)
                    continue;
                const auto& src = std::get<BackgroundImageSource>(prop.Value);
                if (src.Kind != BackgroundImageSource::SourceKind::Path || src.Value.empty())
                    continue;
                std::string norm = UIUtil::NormalizeCssUrlPath(src.Value);
                std::filesystem::path resolved = am->ResolveAssetPathFromReference(norm, importerPath);
                GUID guid = am->GetRegistry().GetAssetGUID(resolved);
                if (guid.IsNull()) {
                    // Opportunistically register assets referenced by CSS so they can resolve at runtime
                    if (std::filesystem::exists(resolved)) {
                        am->GetRegistry().RegisterAsset(resolved);
                        guid = am->GetRegistry().GetAssetGUID(resolved);
                    }
                }
                if (guid.IsNull()) {
                    if (prop.SourceLine > 0) {
                        Logger::Log::Warning("UI: background-image path '{}' not found in '{}' at line {} (resolved '{}')",
                                             src.Value, sourceName, prop.SourceLine, resolved.string());
                    } else {
                        Logger::Log::Warning("UI: background-image path '{}' not found in '{}' (resolved '{}')",
                                             src.Value, sourceName, resolved.string());
                    }
                }
	        }
	    }
}

bool UIStyleAsset::Load() {
    Vector<uint8> data;
    if (!ReadFileBytesShared(GetPath(), data))
    {
        if (!IsLoaded())
            SetState(AssetState::Failed);
        return false;
    }
    return LoadFromData(data);
}

bool UIStyleAsset::ReloadFromData(const Vector<uint8>& data)
{
    return LoadFromData(data);
}

bool UIStyleAsset::LoadFromData(const Vector<uint8>& data)
{
    UIStyleAsset candidate(GetGUID(), GetPath());
    if (!candidate.ParseFromData(data))
    {
        if (!IsLoaded())
            SetState(AssetState::Failed);
        return false;
    }

    const auto handle = candidate.GetStylesheetHandle();
    // No published Stylesheet is mutated. Managers may still be resolving
    // hover against the old snapshot before they process AssetReloaded.
    m_Stylesheet = std::move(candidate.m_Stylesheet);
    m_Segments = std::move(candidate.m_Segments);
    m_ImportedStyleGuids = std::move(candidate.m_ImportedStyleGuids);
    m_Handle = handle;
    SetState(AssetState::Loaded);
    return true;
}

bool UIStyleAsset::ParseFromData(const Vector<uint8>& data)
{
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());

    // Split into segments so we can preserve mid-file @import ordering without expanding text.
    std::vector<ParsedCssSegment> parsed;
    SplitCssIntoSegments(GetPath(), text, parsed);

    // Track transitive @import dependencies as asset GUIDs for hot reload propagation.
    m_ImportedStyleGuids.clear();
    {
        std::vector<std::filesystem::path> importFiles;
        UI::CollectCssImportFiles(GetPath(), text, AssetManager::GetThreadCurrent(), importFiles);

        if (!importFiles.empty())
        {
            AssetManager* am = AssetManager::GetThreadCurrent();
            if (!am)
            {
                // Still allow the stylesheet to load, but we can't map @import dependencies to GUIDs.
                importFiles.clear();
            }
            else
            {
                std::unordered_set<GUID> seen;
                seen.reserve(importFiles.size());
                m_ImportedStyleGuids.reserve(importFiles.size());
                for (const auto& p : importFiles)
                {
                    if (p.empty())
                        continue;
                    GUID g = am->ResolveAssetGuidFromReference(p, GetPath());
                    if (!g.IsNull() && seen.insert(g).second)
                    {
                        m_ImportedStyleGuids.push_back(g);
                    }
                }
            }
        }
    }

    // Parse/refresh segment stylesheets.
    bool ok = true;
    AssetManager* am = AssetManager::GetThreadCurrent();
    const std::string sourceName = GetPath().string();
    const GUID ownerGuid = GetGUID();

    m_Segments.resize(parsed.size());

    Stylesheet firstCss{};
    bool haveFirstCss = false;

    for (size_t i = 0; i < parsed.size(); ++i)
    {
        const ParsedCssSegment& p = parsed[i];
        Segment& seg = m_Segments[i];

        if (p.Kind == ParsedCssSegment::SegmentKind::CssText)
        {
            seg.kind = Segment::Kind::CssText;
            seg.importGuid = GUID::Null();
            Stylesheet sheet{};
            if (!CSSParser::ParseStylesFromString(p.CssText, sheet))
            {
                ok = false;
                continue;
            }

            PostProcessAndWarn(sheet, p.CssText, sourceName, p.StartLine);
            sheet.OwnerAssetGuid = ownerGuid;
            if (!haveFirstCss)
            {
                firstCss = sheet;
                haveFirstCss = true;
            }
            seg.sheet = std::make_shared<const Stylesheet>(std::move(sheet));
        }
        else
        {
            // Import segment
            seg.kind = Segment::Kind::Import;
            seg.importGuid = GUID::Null();
            if (p.ImportResolved.empty())
            {
                ok = false;
                continue;
            }

            if (!am)
            {
                // Without AssetManager context we can't map to GUID. Still keep the segment ordering for later.
                continue;
            }

            GUID g = am->ResolveAssetGuidFromReference(p.ImportResolved, GetPath());
            if (g.IsNull())
            {
                Logger::Log::Warning("UI: CSS @import '{}' could not be mapped to an asset GUID (from '{}')",
                                     p.ImportResolved.string(), sourceName);
                ok = false;
                continue;
            }
            seg.importGuid = g;
        }
    }

    // Single-handle access covers stylesheets without imports. For multi-segment
    // stylesheets (mid-file @import), callers must use GetCascadeHandles().
    m_Stylesheet = haveFirstCss ? firstCss : Stylesheet{};
    m_Stylesheet.OwnerAssetGuid = ownerGuid;
    // A rule-free loaded file still occupies its cascade position. This lets
    // a later save restore its rules without appending it after other styles.
    if (ok && m_Segments.empty())
    {
        Segment empty;
        empty.sheet = std::make_shared<const Stylesheet>(m_Stylesheet);
        m_Segments.push_back(std::move(empty));
    }

    return ok;
}

void UIStyleAsset::Unload()
{
    m_Stylesheet = Stylesheet{};
    m_Segments.clear();
    m_ImportedStyleGuids.clear();
    m_Handle.reset();
    SetState(AssetState::Unloaded);
}

StylesheetHandle UIStyleAsset::GetStylesheetHandle() const
{
    if (!m_Handle)
    {
        auto snapshot = std::make_shared<Stylesheet>(m_Stylesheet);
        snapshot->OwnerAssetGuid = GetGUID();
        m_Handle = std::move(snapshot);
    }
    return m_Handle;
}

std::vector<StylesheetHandle> UIStyleAsset::GetCascadeHandles(AssetManager& assets) const
{
    std::vector<StylesheetHandle> out;
    out.reserve(m_Segments.size());

    std::unordered_set<GUID> stack;
    stack.reserve(32);

    // DFS flattening while preserving import ordering.
    auto append = [&](auto&& self, const UIStyleAsset& cur, int depth) -> void
    {
        constexpr int kMaxDepth = 64;
        if (depth > kMaxDepth)
        {
            Logger::Log::Warning("UI: CSS @import recursion limit exceeded ({}). Skipping remaining imports for '{}'",
                                 kMaxDepth, cur.GetPath().string());
            return;
        }

        const GUID curGuid = cur.GetGUID();
        if (!curGuid.IsNull())
        {
            if (stack.find(curGuid) != stack.end())
            {
                Logger::Log::Warning("UI: CSS @import cycle detected via GUID (skipping): {}", curGuid.ToString());
                return;
            }
            stack.insert(curGuid);
        }

        for (const Segment& seg : cur.m_Segments)
        {
            if (seg.kind == Segment::Kind::CssText)
            {
                if (seg.sheet)
                    out.push_back(seg.sheet);
                continue;
            }

            // Import segment
            if (seg.importGuid.IsNull())
                continue;

            if (stack.find(seg.importGuid) != stack.end())
            {
                Logger::Log::Warning("UI: CSS @import cycle detected via GUID (skipping): {} imports {}", curGuid.ToString(), seg.importGuid.ToString());
                continue;
            }

            auto importedAsset = assets.GetAsset(seg.importGuid);
            if (!importedAsset)
            {
                // A missing import silently drops its rules from the cascade —
                // when the sheet is what hides an element (modal overlays),
                // the symptom is visual corruption with no log trail. Say so.
                Logger::Log::Warning(
                    "UI: CSS @import in '{}' unresolved (GUID {}) — its rules are "
                    "absent from the cascade this attach",
                    cur.GetPath().string(), seg.importGuid.ToString());
                // Best-effort: request load, but do not block here.
                assets.LoadAsset(seg.importGuid, AssetLoadResultCallback{}, AssetLoadPriority::Low);
                continue;
            }
            if (importedAsset->GetType() != AssetType::UIStyle)
            {
                continue;
            }

            auto* importedStyle = static_cast<UIStyleAsset*>(importedAsset.get());
            if (importedStyle && importedStyle->IsLoaded() && !importedStyle->HasFailed())
            {
                self(self, *importedStyle, depth + 1);
            }
        }

        if (!curGuid.IsNull())
            stack.erase(curGuid);
    };

    append(append, *this, /*depth=*/0);
    return out;
}
