#include "UI/Internal/AttachDetachInternal.h"
#include "UI/UIHotReload.h"
#include "UI/UIManager.h"
#include "UI/UITextureRegistry.h"
#include "UIManager_Internal.h"

#include <cassert>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetEvents.h"
#include <chrono>
#include "Assets/TextureAsset.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Mount.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleUtil.h"
#include "UI/UIElement.h"
#include "UI/UITemplateNode.h"
#include "UI/UiContext.h"
#include "UIAttributeAccess.h"
#include "Assets/CssImports.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::UIRegistration;

namespace
{
// Minimal deep-clone from a template node: creates live UIElement subclass instances
// via the factory registry, copies attributes/classes, and re-applies attribute bindings.
static std::unique_ptr<UIElement> CloneElement(const UITemplateNode& src)
{
    const std::string& tag = src.TagName;
    auto out = ElementFactoryRegistry::Instance().Create(tag);
    if (!out)
    {
        // An unregistered tag is still an identity: stamping it is what lets the element
        // reconcile against its own tag on the next reload instead of being rebuilt.
        out = std::make_unique<UIElement>();
        UIAttributeAccess::SetCreatedTag(*out, tag);
    }

    if (!src.Id.empty())
        out->SetId(src.Id);
    for (const auto& cls : src.Classes)
        out->AddClass(cls);
    // Copy authored attributes first without dirty marks; we'll re-apply bindings below.
    for (const auto& kv : src.Attributes)
    {
        UIAttributeAccess::SetAuthoredAttribute(*out, kv.first, kv.second, false);
        if (kv.first == "style" && kv.second.find(':') != std::string::npos)
            UIAttributeAccess::SetInlineStyleAttribute(*out, kv.second);
    }

    // Re-apply attribute bindings so derived controls (e.g., Label) pick up properties.
    // Template attributes already have lowercase keys.
    std::string innerText;
    if (const std::string* t = src.FindAttribute("text"))
        innerText = *t;
    ElementFactoryRegistry::Instance().ApplyAttributes(*out, src.Attributes, innerText);

    // Recurse
    for (const auto& child : src.Children)
    {
        out->AddChild(CloneElement(*child));
    }
    return out;
}

static void StampHotReloadBinding(UIElement& element, uint64 bindingId)
{
    element.SetHotReloadBindingId(bindingId);
    for (const auto& child : element.GetChildren())
    {
        if (child)
            StampHotReloadBinding(*child, bindingId);
    }
}

static std::unordered_map<std::string, int> BuildCssUrlLineMap(const std::string& cssText)
{
    std::unordered_map<std::string, int> m;
    size_t pos = 0;
    auto lineOf = [&](size_t off)
    {
        int ln = 1;
        for (size_t i = 0; i < cssText.size() && i < off; ++i)
        {
            if (cssText[i] == '\n')
                ++ln;
        }
        return ln;
    };
    while (true)
    {
        pos = cssText.find("url(", pos);
        if (pos == std::string::npos)
            break;
        size_t i = pos + 4;
        while (i < cssText.size() && (cssText[i] == ' ' || cssText[i] == '\t' || cssText[i] == '\r' || cssText[i] == '\n'))
            ++i;
        bool quoted = false;
        char qc = 0;
        if (i < cssText.size() && (cssText[i] == '\'' || cssText[i] == '\"'))
        {
            quoted = true;
            qc = cssText[i++];
        }
        size_t start = i;
        while (i < cssText.size())
        {
            char c = cssText[i];
            if (quoted)
            {
                if (c == qc)
                    break;
            }
            else if (c == ')')
                break;
            ++i;
        }
        std::string inner = cssText.substr(start, i - start);
        auto trim = [&](std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
                ++a;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
                --b;
            s = s.substr(a, b - a);
        };
        trim(inner);
        if (!inner.empty() && !m.count(inner))
            m.emplace(inner, lineOf(pos));
        pos = (i < cssText.size()) ? i + 1 : i;
    }
    return m;
}

static std::string MakeBackgroundPathCacheKey(std::string_view path, std::string_view sourceAlias)
{
    if (sourceAlias.empty())
        return std::string(path);

    std::string key;
    key.reserve(sourceAlias.size() + 1u + path.size());
    key.append(sourceAlias);
    key.push_back('|');
    key.append(path);
    return key;
}

struct ParsedCssFileResult
{
    bool ok = false;
    std::string path;
    Stylesheet sheet{};
    std::unordered_map<std::string, int> urlLineMap;
};

struct ParsedLayoutFileResult
{
    bool ok = false;
    std::string path;
    std::unique_ptr<UIElement> root;
};

} // namespace

void UIManager::LoadLayout(const GUID& guid)
{
    if (!m_AssetManager)
    {
        Logger::Log::Warning("UI: AssetManager not set; GUID-based loading disabled");
        return;
    }
    auto asset = m_AssetManager->GetAsset(guid);
    if (!asset)
    {
        Logger::Log::Warning("UI: UILayout asset not found for GUID {}", guid.ToString());
        return;
    }
    if (asset->GetType() != AssetType::UILayout)
    {
        Logger::Log::Warning("UI: Asset GUID {} is not a UILayout", guid.ToString());
        return;
    }
    auto* layout = static_cast<UILayoutAsset*>(asset.get());
    if (!layout->IsLoaded())
    {
        if (layout->Load())
            layout->PostLoad();
    }
    if (!layout->IsLoaded() || layout->HasFailed())
    {
        Logger::Log::Warning("UI: Failed to load UILayout asset GUID {}", guid.ToString());
        return;
    }
    if (auto* root = layout->GetRoot())
    {
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
        ResetRetainedYogaTree();
#endif
        AdoptRoot(CloneElement(*root));
        if (m_Root)
            m_Root->SetOwnerManager(this);
        if (m_HotReload && m_Root)
        {
            m_HotReload->RegisterLayoutBinding(guid, m_Root.get(), UIHotReload::LayoutBindMode::ReconcileSelf);
        }
    }
}

void UIManager::AttachStyle(const GUID& guid)
{
    if (!m_AssetManager)
    {
        Logger::Log::Warning("UI: AssetManager not set; GUID-based loading disabled");
        return;
    }
    auto asset = m_AssetManager->GetAsset(guid);
    if (!asset)
    {
        Logger::Log::Warning("UI: UIStyle asset not found for GUID {}", guid.ToString());
        return;
    }
    if (asset->GetType() != AssetType::UIStyle)
    {
        Logger::Log::Warning("UI: Asset GUID {} is not a UIStyle", guid.ToString());
        return;
    }
    auto* style = static_cast<UIStyleAsset*>(asset.get());
    if (!style->IsLoaded())
    {
        if (style->Load())
            style->PostLoad();
    }
    if (!style->IsLoaded() || style->HasFailed())
    {
        Logger::Log::Warning("UI: Failed to load UIStyle asset GUID {}", guid.ToString());
        return;
    }

    // Ensure imported styles are loaded so GetCascadeHandles() can resolve them immediately.
    // This runs on the UI thread and only happens on attach, not per-frame.
    for (const GUID& dep : style->GetImportedStyleGuids())
    {
        if (dep.IsNull() || !m_AssetManager || m_AssetManager->IsAssetLoaded(dep))
            continue;
        auto handle = m_AssetManager->LoadAsset(dep, AssetLoadResultCallback{}, AssetLoadPriority::High);
        if (handle.Future.has_value())
        {
            handle.Future->wait();
        }
    }

    // Attach full cascade (including @imports) preserving in-file ordering.
    std::vector<StylesheetHandle> cascade = style->GetCascadeHandles(*m_AssetManager);
    if (cascade.empty())
    {
        // Fallback to legacy single handle (best-effort).
        cascade.push_back(style->GetStylesheetHandle());
    }

    // Reattachment can precede the queued reload event. Adopt an existing
    // binding first so the current handles do not stack beside old snapshots.
    if (m_HotReload)
        m_HotReload->ApplyStyleNow(guid);
    bool anyAdded = false;
    for (const auto& h : cascade)
    {
        if (!h)
            continue;
        const Stylesheet* ptr = h.get();
        auto it = std::find_if(m_GlobalStylesheets.begin(), m_GlobalStylesheets.end(),
                               [&](const StylesheetHandle& existing)
                               { return existing && existing.get() == ptr; });
        if (it == m_GlobalStylesheets.end())
        {
            m_GlobalStylesheets.push_back(h);
            anyAdded = true;
        }
    }
    if (anyAdded)
    {
        ++m_StylesheetSetGeneration;
        InvalidateAllInternedSheetSets();
    }

    if (m_HotReload)
    {
        m_HotReload->RegisterStyleBinding(guid, /*target=*/nullptr, UIHotReload::StyleBindMode::Global);
    }
}

bool UIManager::LoadLayoutFromAsset(const UILayoutAsset& asset)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    auto* root = asset.GetRoot();
    if (!root)
    {
        Logger::Log::Warning("UI: UILayoutAsset has no root");
        return false;
    }
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    ResetRetainedYogaTree();
#endif
    AdoptRoot(CloneElement(*root));
    if (m_Root)
        m_Root->SetOwnerManager(this);

    // Track this layout as asset-driven so UIHotReload can reconcile it in-place on AssetReloaded.
    // On initial load, skip the full reconciliation pass (ApplyLayoutNow) since CloneElement
    // already produced correct output. Defer it to after the first frame so binding IDs are
    // still stamped for subsequent hot-reloads, just not on the critical startup path.
    if (m_HotReload && m_Root)
    {
        m_HotReload->RegisterLayoutBinding(asset.GetGUID(), m_Root.get(), UIHotReload::LayoutBindMode::ReconcileSelf);
        GUID guid = asset.GetGUID();
        UIHotReload* hotReload = m_HotReload.get();
        m_Root->PostAction([hotReload, guid]() {
            hotReload->ApplyLayoutNow(guid);
        });
    }

    return m_Root != nullptr;
}

bool UIManager::AttachStyleFromAsset(const UIStyleAsset& asset)
{
    auto MsSince = [](const auto& t) {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    };

    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    std::vector<StylesheetHandle> cascade;
    if (m_AssetManager)
    {
        // Kick off all import loads in parallel, then wait once for all to complete.
        auto tImports = std::chrono::high_resolution_clock::now();
        std::vector<AssetLoadHandle> pendingLoads;
        size_t alreadyLoaded = 0;
        for (const GUID& dep : asset.GetImportedStyleGuids())
        {
            if (dep.IsNull())
                continue;
            if (m_AssetManager->IsAssetLoaded(dep))
            {
                ++alreadyLoaded;
                continue;
            }
            pendingLoads.push_back(
                m_AssetManager->LoadAsset(dep, AssetLoadResultCallback{}, AssetLoadPriority::High));
        }
        for (auto& load : pendingLoads)
        {
            if (load.Future.has_value())
                load.Future->wait();
        }
        Logger::Log::Info("[Startup]       CSS imports: {:.1f}ms ({} loaded, {} were cached)",
                         MsSince(tImports), pendingLoads.size(), alreadyLoaded);

        auto tCascade = std::chrono::high_resolution_clock::now();
        cascade = asset.GetCascadeHandles(*m_AssetManager);
        Logger::Log::Info("[Startup]       GetCascadeHandles: {:.1f}ms ({} handles)", MsSince(tCascade), cascade.size());
        // The import list is transitive, so a fully-resolved cascade has one
        // handle per import plus the root sheet. A shortfall means whole
        // stylesheets are missing from this attach (rules like the modal
        // overlays' display:none simply won't exist) — say so, because the
        // visual symptom carries no log trail of its own.
        const size_t expected = asset.GetImportedStyleGuids().size() + 1;
        if (cascade.size() < expected)
            Logger::Log::Warning(
                "UI: stylesheet cascade for '{}' resolved {} of {} sheets — "
                "unresolved @imports are absent from this attach",
                asset.GetPath().string(), cascade.size(), expected);
    }
    if (cascade.empty())
        cascade.push_back(asset.GetStylesheetHandle());

    // Adopt a prior binding before current snapshots are deduplicated/appended.
    if (m_HotReload)
        m_HotReload->ApplyStyleNow(asset.GetGUID());
    bool anyAdded = false;
    for (const auto& h : cascade)
    {
        if (!h)
            continue;

        // Idempotent attach: do not reorder on hot-reload (precedence stays stable).
        const Stylesheet* ptr = h.get();
        auto it = std::find_if(m_GlobalStylesheets.begin(), m_GlobalStylesheets.end(),
                               [&](const StylesheetHandle& existing)
                               { return existing && existing.get() == ptr; });
        if (it == m_GlobalStylesheets.end())
        {
            m_GlobalStylesheets.push_back(h);
            anyAdded = true;
        }
    }
    if (anyAdded)
    {
        ++m_StylesheetSetGeneration;
        InvalidateAllInternedSheetSets();
    }

    // Track this style as asset-driven so UIHotReload can mark dirty on AssetReloaded(UIStyle).
    if (m_HotReload)
    {
        m_HotReload->RegisterStyleBinding(asset.GetGUID(), /*target=*/nullptr, UIHotReload::StyleBindMode::Global);
    }

    return true;
}

bool UIManager::BindLayoutToSubtreeChildrenFromAsset(UIElement* target, const UILayoutAsset& asset)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!target || !m_HotReload)
        return false;

    // Ensure this subtree is tracked for hot reload and apply immediately.
    const uint64 bindingId = m_HotReload->RegisterLayoutBinding(
        asset.GetGUID(), target, UIHotReload::LayoutBindMode::ReconcileChildren);
    m_HotReload->ApplyLayoutNow(asset.GetGUID());

    // Inactive dock tabs are deliberately detached from the live root. The hot
    // reload reconciler cannot discover them by instance id until activation,
    // but first-mount preparation needs their subtree immediately. Seed the
    // detached target directly and stamp it with the registered binding id so
    // normal reconciliation takes over once the tab is mounted.
    if (target->GetOwnerManager() != this)
    {
        const UITemplateNode* root = asset.GetRoot();
        if (!root)
            return false;
        for (const auto& cls : root->Classes)
        {
            if (!cls.empty())
                target->AddClass(cls);
        }
        for (const auto& childTemplate : root->Children)
        {
            if (!childTemplate)
                continue;
            auto child = CloneElement(*childTemplate);
            StampHotReloadBinding(*child, bindingId);
            target->AddChild(std::move(child));
        }
    }
    return true;
}

bool UIManager::AttachStyleToSubtreeFromAsset(UIElement* target, const UIStyleAsset& asset)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!target)
        return false;

    std::vector<StylesheetHandle> cascade;
    if (m_AssetManager)
    {
        // Kick off all import loads in parallel, then wait once for all to complete.
        std::vector<AssetLoadHandle> pendingLoads;
        for (const GUID& dep : asset.GetImportedStyleGuids())
        {
            if (dep.IsNull() || m_AssetManager->IsAssetLoaded(dep))
                continue;
            pendingLoads.push_back(
                m_AssetManager->LoadAsset(dep, AssetLoadResultCallback{}, AssetLoadPriority::High));
        }
        for (auto& load : pendingLoads)
        {
            if (load.Future.has_value())
                load.Future->wait();
        }
        cascade = asset.GetCascadeHandles(*m_AssetManager);
    }
    if (cascade.empty())
        cascade.push_back(asset.GetStylesheetHandle());
    if (cascade.empty())
        return false;

    if (m_HotReload)
        m_HotReload->ApplyStyleNow(asset.GetGUID());
    // Attach to subtree and register hot reload.
    const auto& existing = target->GetStylesheets();
    for (const auto& h : cascade)
    {
        if (!h)
            continue;
        const Stylesheet* ptr = h.get();
        const bool already = std::any_of(existing.begin(), existing.end(),
                                         [&](const StylesheetHandle& s)
                                         { return s && s.get() == ptr; });
        if (!already)
            target->AddStylesheet(h);
    }
    if (m_HotReload)
    {
        m_HotReload->RegisterStyleBinding(asset.GetGUID(), target, UIHotReload::StyleBindMode::Subtree);
    }

    // Ensure styles are recomputed for this subtree immediately.
    MarkStyleDirtySubtree(target);
    return true;
}

void UIManager::UnregisterSubtreeStyleBinding(UIElement* target, const GUID& styleGuid)
{
    if (m_HotReload)
        m_HotReload->UnregisterStyleBinding(styleGuid, target);
}

UIManager::StyleImportStatus UIManager::QueryStyleImports(const UIStyleAsset& asset)
{
    if (!m_AssetManager)
        return StyleImportStatus::Resident; // no manager → AttachStyle uses the bare stylesheet
    StyleImportStatus status = StyleImportStatus::Resident;
    for (const GUID& dep : asset.GetImportedStyleGuids())
    {
        if (dep.IsNull() || m_AssetManager->IsAssetLoaded(dep))
            continue;
        if (m_AssetManager->IsLoadSuppressed(dep))
            return StyleImportStatus::Failed; // permanently-failed import: let the caller latch
        status = StyleImportStatus::Pending;
        // Kick the load (non-blocking) so it's resident for a later retry, mirroring
        // AttachStyleToSubtreeFromAsset's request — minus the blocking Future->wait().
        (void)m_AssetManager->LoadAsset(dep, AssetLoadResultCallback{}, AssetLoadPriority::High);
    }
    return status;
}

void UIManager::ClearSubtreeStyle(UIElement* target, const GUID& styleGuid)
{
    if (!target)
        return;
    // Order matters: drop the old style's hot-reload binding first (so a later hot-edit
    // of the old asset can't re-apply its cascade), then remove the subtree's sheets.
    // st.Root-style subtrees carry only the previous style's cascade, so clearing all
    // of them is exactly the swap; runtime-added sheets (if any) would also go, which
    // is correct for a full style replace.
    if (!styleGuid.IsNull())
        UnregisterSubtreeStyleBinding(target, styleGuid);
    const auto prevSheets = target->GetStylesheets(); // copy before mutating
    for (const auto& sheet : prevSheets)
        target->RemoveStylesheet(sheet);
}

bool UIManager::AttachStyleToSubtreeFromAssetPath(UIElement* target, const std::string& assetPath)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!target || assetPath.empty() || !m_AssetManager)
        return false;

    const std::filesystem::path resolved = m_AssetManager->ResolveAssetPath(assetPath);
    if (resolved.empty())
        return false;

    GUID g = m_AssetManager->GetRegistry().GetAssetGUID(resolved);
    if (g.IsNull())
        return false;
    if (!m_AssetManager->IsAssetLoaded(g))
    {
        auto h = m_AssetManager->LoadAsset(g, AssetLoadResultCallback{}, AssetLoadPriority::High);
        if (h.Future.has_value())
            h.Future->wait();
    }

    auto a = m_AssetManager->GetAsset(g);
    if (!a || a->GetType() != AssetType::UIStyle || !a->IsLoaded() || a->HasFailed())
        return false;

    auto* style = static_cast<UIStyleAsset*>(a.get());
    if (!style)
        return false;

    return AttachStyleToSubtreeFromAsset(target, *style);
}

namespace
{
// Resolves an asset-relative path to its registry GUID, registering the file
// on demand. Built-in controls request their layouts and sheets by path, and a
// mounted editor tree is not always scanned before the first request lands —
// a persisted registry from an older build in particular knows nothing about
// a file that build did not ship.
GUID ResolveAssetGuidRegisteringOnDemand(AssetManager& assets, const std::string& assetPath,
                                         std::filesystem::path* outResolved)
{
    const std::filesystem::path resolved = assets.ResolveAssetPath(assetPath);
    if (outResolved)
        *outResolved = resolved;
    if (resolved.empty())
        return GUID{};
    GUID g = assets.GetRegistry().GetAssetGUID(resolved);
    if (!g.IsNull())
        return g;
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec) || !std::filesystem::is_regular_file(resolved, ec))
        return GUID{};
    (void)assets.GetRegistry().RegisterAsset(resolved);
    g = assets.GetRegistry().GetAssetGUID(resolved);
    if (!g.IsNull())
        Logger::Log::Info("UI: '{}' resolved to '{}' but was not registered; registered on demand (guid={})",
                          assetPath, resolved.string(), g.ToString());
    return g;
}
} // namespace

bool UIManager::InstantiateLayoutChildrenFromAssetPath(UIElement* target, const std::string& assetPath)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!target || assetPath.empty() || !m_AssetManager)
        return false;

    std::filesystem::path resolved;
    const GUID g = ResolveAssetGuidRegisteringOnDemand(*m_AssetManager, assetPath, &resolved);
    if (g.IsNull())
    {
        Logger::Log::Warning("UI: layout '{}' resolved to '{}' but has no AssetRegistry GUID (not instantiated)",
                             assetPath, resolved.string());
        return false;
    }
    if (!m_AssetManager->IsAssetLoaded(g))
    {
        auto h = m_AssetManager->LoadAsset(g, AssetLoadResultCallback{}, AssetLoadPriority::High);
        if (h.Future.has_value())
            h.Future->wait();
    }

    auto a = m_AssetManager->GetAsset(g);
    if (!a || a->GetType() != AssetType::UILayout || !a->IsLoaded() || a->HasFailed())
        return false;

    const UITemplateNode* root = static_cast<UILayoutAsset*>(a.get())->GetRoot();
    if (!root)
        return false;
    for (const auto& cls : root->Classes)
    {
        if (!cls.empty())
            target->AddClass(cls);
    }
    for (const auto& childTemplate : root->Children)
    {
        if (childTemplate)
            target->AddChild(CloneElement(*childTemplate));
    }
    return true;
}

void UIManager::NotifyElementOwnerChanged(UIElement* el)
{
    if (!el)
        return;

    // instanceId -> element map (slice E1): insert on owner-gain. Erase
    // happens in the SetOwnerManager detach branch + ~UIElement.
    m_ElementsByInstanceId[el->GetInstanceId()] = el;

    if (!m_AssetManager)
        return;

    // Diagnostics: log once per resolved stylesheet path when we have to auto-register it
    // because it wasn't already present in the AssetRegistry.
    //
    // This helps detect platform/timing issues where mounted Editor assets aren't scanned early
    // enough for control-driven styling (e.g., macOS user assets root + polling file watcher).

    // 1) Attach any cached resolved style GUIDs (no path resolution).
    for (const GUID& g : el->GetRequestedSubtreeStyleAssetGuids())
    {
        if (g.IsNull())
            continue;
        auto a = m_AssetManager->GetAsset(g);
        if (!a || !a->IsLoaded() || a->HasFailed())
        {
            auto h = m_AssetManager->LoadAsset(g, AssetLoadResultCallback{}, AssetLoadPriority::High);
            if (h.Future.has_value())
                h.Future->wait();
            a = m_AssetManager->GetAsset(g);
        }
        if (a && a->GetType() == AssetType::UIStyle && a->IsLoaded() && !a->HasFailed())
        {
            (void)AttachStyleToSubtreeFromAsset(el, *static_cast<UIStyleAsset*>(a.get()));
        }
    }

    // 2) Consume any newly requested style paths, resolve+attach once, then cache by GUID on the element.
    auto pending = el->ConsumeRequestedSubtreeStyleAssetPaths();
    for (const auto& request : pending)
    {
        const auto& p = request.Path;
        if (p.empty())
            continue;
        std::filesystem::path resolved;
        // Explicit paths remain authoritative. For implicit control paths, a
        // mounted owning source must not fall back to a project's override,
        // even if the shipped file is missing.
        const bool pinSource = !request.SourceAlias.empty() && p.find(':') == std::string::npos &&
                               !std::filesystem::path(p).is_absolute() &&
                               !m_AssetManager->GetSourceRoot(request.SourceAlias).empty();
        const std::string stylePath = pinSource ? request.SourceAlias + ":" + p : p;
        const GUID g = ResolveAssetGuidRegisteringOnDemand(*m_AssetManager, stylePath, &resolved);
        if (resolved.empty())
            continue;
        if (g.IsNull())
        {
            // Best-effort diagnostics: we could resolve to a concrete file on disk, but it does not
            // have a registry identity, so AttachStyleToSubtreeFromAssetPath will fail.
            Logger::Log::Warning("UI: requested subtree style '{}' resolved to '{}' but has no AssetRegistry GUID (style will not attach)",
                                 p, resolved.string());
            continue;
        }
        if (AttachStyleToSubtreeFromAssetPath(el, stylePath))
        {
            el->AddRequestedSubtreeStyleGuid(g);
        }
    }
}

bool UIManager::LoadLayoutFromFile(const std::string& path)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    std::unique_ptr<UIElement> parsed;
    if (!UIParsing::XMLParser::ParseLayoutFromFile(path, parsed))
        return false;
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    ResetRetainedYogaTree();
#endif
    AdoptRoot(std::move(parsed));
    if (m_Root)
        m_Root->SetOwnerManager(this);
    return m_Root != nullptr;
}

void UIManager::LoadLayoutFromFileAsync(const std::string& path)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!m_JobSystem)
    {
        (void)LoadLayoutFromFile(path);
        return;
    }

    const uint64_t gen = ++m_AsyncLayoutParseGeneration;
    // Shared, so the completion can post from a worker after this manager is gone: the closed
    // dispatcher refuses it. Posted work runs only from this manager's own drains.
    std::shared_ptr<UI::UiDispatcher> dispatcher = m_Dispatcher;
    JobSystem::WorkStealingThreadPool* js = m_JobSystem;
    const std::string pathCopy = path;

    JobSystem::TaskHandle handle = js->Submit([pathCopy]() -> std::shared_ptr<ParsedLayoutFileResult>
                                              {
                                                 auto res = std::make_shared<ParsedLayoutFileResult>();
                                                 res->path = pathCopy;
                                                 std::unique_ptr<UIElement> parsed;
                                                 res->ok = UIParsing::XMLParser::ParseLayoutFromFile(pathCopy, parsed);
                                                 if (res->ok)
                                                     res->root = std::move(parsed);
                                                 return res; });

    handle.OnComplete([this, dispatcher, gen](const JobSystem::TaskHandle& h)
                      {
                          std::shared_ptr<ParsedLayoutFileResult> res;
                          if (!h.TryGetResult(res) || !res)
                              return;
                          dispatcher->Post([this, gen, res]()
                                           {
                                               if (!m_AppliesAsyncResults)
                                                   return;
                                               if (gen != m_AsyncLayoutParseGeneration)
                                                   return; // superseded by newer request
                                               if (!res->ok || !res->root)
                                               {
                                                   Logger::Log::Warning("UI: Async layout parse failed for '{}'", res->path);
                                                   return;
                                               }
                                               Logger::Log::Info("UI: Async layout parsed '{}'", res->path);
                                               SetRoot(std::move(res->root));
                                           }); });
    handle.OnFailure([dispatcher, pathCopy](const JobSystem::String& err)
                     {
                         dispatcher->Post([pathCopy, err]()
                                          { Logger::Log::Warning("UI: Async layout parse failed for '{}': {}", pathCopy, err); }); });
}

bool UIManager::AttachStyleFromFile(const std::string& path)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    auto bytes = Rendering::Utils::ReadFile(path);
    if (bytes.empty())
        return false;
    // Read stylesheet and parse
    Logger::Log::Info("UI: Read stylesheet ({} bytes) from '{}'", (uint32_t)bytes.size(), path);
    std::string css(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string expanded;
    std::vector<std::filesystem::path> importFiles;
    UI::ExpandCssImports(std::filesystem::path(path), css, m_AssetManager, expanded, importFiles);

    Stylesheet sheet{};
    if (UIParsing::CSSParser::ParseStylesFromString(expanded.empty() ? css : expanded, sheet))
    {
        // Record source name for diagnostics
        sheet.SourceName = path;
        UIParsing::WarnUnknownStylesheetProperties(sheet);
        // Build url(...) -> line map (best effort)
        auto urlLineMap = BuildCssUrlLineMap(expanded.empty() ? css : expanded);
        if (m_AssetManager)
        {
            auto normalize = [](const std::string& p)
            { return UIUtil::NormalizeCssUrlPath(p); };
            // Pre-resolve background-image paths and warn once at load/hot-reload
            for (auto& rule : sheet.Rules)
            {
                for (auto& prop : rule.Properties)
                {
                    if (prop.PropertyId != StylePropertyId::BackgroundImage)
                        continue;
                    const auto& src = std::get<BackgroundImageSource>(prop.Value);
                    if (src.Kind != BackgroundImageSource::SourceKind::Path || src.Value.empty())
                        continue;

                    auto itL = urlLineMap.find(src.Value);
                    if (itL != urlLineMap.end())
                        prop.SourceLine = itL->second;
                    std::string norm = normalize(src.Value);
                    auto resolved = m_AssetManager->ResolveAssetPath(norm);
                    GUID g = m_AssetManager->GetRegistry().GetAssetGUID(resolved);
                    if (g.IsNull())
                    {
                        if (prop.SourceLine > 0)
                        {
                            Logger::Log::Warning("UI: background-image path '{}' not found in '{}' at line {} (resolved '{}')", src.Value, path, prop.SourceLine, resolved.string());
                        }
                        else
                        {
                            Logger::Log::Warning("UI: background-image path '{}' not found in '{}' (resolved '{}')", src.Value, path, resolved.string());
                        }
                    }
                }
            }
        }
        Logger::Log::Info("UI: Stylesheet parsed ({} rules)", (uint32_t)sheet.Rules.size());
        const std::string key = UI::NormalizeStylesheetPathKey(path);
        auto itS = m_FileStylesheets.find(key);
        if (itS == m_FileStylesheets.end())
        {
            auto shared = std::make_shared<Stylesheet>(sheet);
            m_FileStylesheets.emplace(key, shared);
            StylesheetHandle handle = shared;
            m_GlobalStylesheets.push_back(handle);
            ++m_StylesheetSetGeneration;
            InvalidateAllInternedSheetSets();
        }
        else
        {
            // Hot reload: update the existing stylesheet object in-place so removed
            // properties/rules revert correctly and precedence stays stable.
            if (itS->second)
            {
                *(itS->second) = sheet;
            }
            // Defensive: ensure the handle is attached globally (without reordering).
            const Stylesheet* ptr = itS->second.get();
            auto it = std::find_if(m_GlobalStylesheets.begin(), m_GlobalStylesheets.end(),
                                   [&](const StylesheetHandle& existing)
                                   { return existing && existing.get() == ptr; });
            if (it == m_GlobalStylesheets.end())
            {
                m_GlobalStylesheets.push_back(itS->second);
            }
            NotifyStylesheetContentChanged();
            // File-based stylesheets participate globally, so conservatively mark the full tree dirty.
            MarkStyleDirtyAll();
        }
        return true;
    }
    Logger::Log::Warning("UI: Stylesheet parse failed for '{}'", path);
    return false;
}

void UIManager::AttachStyleFromFileAsync(const std::string& path)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!m_JobSystem)
    {
        (void)AttachStyleFromFile(path);
        return;
    }

    const std::string key = UI::NormalizeStylesheetPathKey(path);
    const uint64_t gen = ++m_AsyncStyleParseGeneration[key];

    // Shared, so the completion can post from a worker after this manager is gone: the closed
    // dispatcher refuses it. Posted work runs only from this manager's own drains.
    std::shared_ptr<UI::UiDispatcher> dispatcher = m_Dispatcher;
    JobSystem::WorkStealingThreadPool* js = m_JobSystem;
    AssetManager* assets = m_AssetManager;
    const std::string pathCopy = path;

    JobSystem::TaskHandle handle = js->Submit([pathCopy, assets]() -> std::shared_ptr<ParsedCssFileResult>
                                              {
                                                 auto res = std::make_shared<ParsedCssFileResult>();
                                                 res->path = pathCopy;
                                                 auto bytes = Rendering::Utils::ReadFile(pathCopy);
                                                 if (bytes.empty())
                                                 {
                                                     res->ok = false;
                                                     return res;
                                                 }
                                                 std::string css(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                                                 std::string expanded;
                                                 std::vector<std::filesystem::path> importFiles;
                                                 UI::ExpandCssImports(std::filesystem::path(pathCopy), css, assets, expanded, importFiles);

                                                 Stylesheet sheet{};
                                                 if (!UIParsing::CSSParser::ParseStylesFromString(expanded.empty() ? css : expanded, sheet))
                                                 {
                                                     res->ok = false;
                                                     return res;
                                                 }
                                                 sheet.SourceName = pathCopy;
                                                 res->urlLineMap = BuildCssUrlLineMap(expanded.empty() ? css : expanded);
                                                 res->sheet = std::move(sheet);
                                                 res->ok = true;
                                                 return res; });

    handle.OnComplete([this, dispatcher, key, gen](const JobSystem::TaskHandle& h)
                      {
                          std::shared_ptr<ParsedCssFileResult> res;
                          if (!h.TryGetResult(res) || !res)
                              return;
                          dispatcher->Post([this, key, gen, res]()
                                           {
                                               if (!m_AppliesAsyncResults)
                                                   return;
                                               auto itGen = m_AsyncStyleParseGeneration.find(key);
                                               if (itGen != m_AsyncStyleParseGeneration.end() && itGen->second != gen)
                                                   return; // superseded by newer request for this file

                                               if (!res->ok)
                                               {
                                                   Logger::Log::Warning("UI: Async stylesheet parse failed for '{}'", res->path);
                                                   return;
                                               }

                                               Stylesheet sheet = std::move(res->sheet);
                                               const auto& urlLineMap = res->urlLineMap;

                                               if (m_AssetManager)
                                               {
                                                   auto normalize = [](const std::string& p)
                                                   { return UIUtil::NormalizeCssUrlPath(p); };
                                                   // Pre-resolve background-image paths and warn once at load/hot-reload
                                                   for (auto& rule : sheet.Rules)
                                                   {
                                                      for (auto& prop : rule.Properties)
                                                      {
                                                          if (prop.PropertyId != StylePropertyId::BackgroundImage)
                                                              continue;
                                                          const auto& src = std::get<BackgroundImageSource>(prop.Value);
                                                          if (src.Kind != BackgroundImageSource::SourceKind::Path || src.Value.empty())
                                                              continue;

                                                          auto itL = urlLineMap.find(src.Value);
                                                          if (itL != urlLineMap.end())
                                                              prop.SourceLine = itL->second;
                                                          std::string norm = normalize(src.Value);
                                                          auto resolved = m_AssetManager->ResolveAssetPath(norm);
                                                          GUID g = m_AssetManager->GetRegistry().GetAssetGUID(resolved);
                                                          if (g.IsNull())
                                                          {
                                                              if (prop.SourceLine > 0)
                                                              {
                                                                  Logger::Log::Warning("UI: background-image path '{}' not found in '{}' at line {} (resolved '{}')",
                                                                                      src.Value, res->path, prop.SourceLine, resolved.string());
                                                              }
                                                              else
                                                              {
                                                                  Logger::Log::Warning("UI: background-image path '{}' not found in '{}' (resolved '{}')",
                                                                                      src.Value, res->path, resolved.string());
                                                              }
                                                          }
                                                      }
                                                   }
                                               }

                                               Logger::Log::Info("UI: Async stylesheet parsed ({} rules)", (uint32_t)sheet.Rules.size());

                                               const std::string mapKey = UI::NormalizeStylesheetPathKey(res->path);
                                               auto itS = m_FileStylesheets.find(mapKey);
                                               if (itS == m_FileStylesheets.end())
                                               {
                                                   auto shared = std::make_shared<Stylesheet>(sheet);
                                                   m_FileStylesheets.emplace(mapKey, shared);
                                                   StylesheetHandle handle = shared;
                                                   m_GlobalStylesheets.push_back(handle);
                                                   ++m_StylesheetSetGeneration;
                                                   InvalidateAllInternedSheetSets();
                                               }
                                               else
                                               {
                                                   // Hot reload: update the existing stylesheet object in-place so removed
                                                   // properties/rules revert correctly and precedence stays stable.
                                                   if (itS->second)
                                                   {
                                                       *(itS->second) = sheet;
                                                   }
                                                   // Defensive: ensure the handle is attached globally (without reordering).
                                                   const Stylesheet* ptr = itS->second.get();
                                                   auto it = std::find_if(m_GlobalStylesheets.begin(), m_GlobalStylesheets.end(),
                                                                          [&](const StylesheetHandle& existing) { return existing && existing.get() == ptr; });
                                                   if (it == m_GlobalStylesheets.end())
                                                   {
                                                       m_GlobalStylesheets.push_back(itS->second);
                                                   }
                                                   NotifyStylesheetContentChanged();
                                                   MarkStyleDirtyAll();
                                               }
                                           }); });
    handle.OnFailure([dispatcher, pathCopy](const JobSystem::String& err)
                     {
                         dispatcher->Post([pathCopy, err]()
                                          { Logger::Log::Warning("UI: Async stylesheet parse failed for '{}': {}", pathCopy, err); }); });
}

void UIManager::AddStylesheet(const StylesheetHandle& sheet)
{
    if (!sheet)
        return;
    // Re-adding a present sheet is a position-preserving no-op. Cascade
    // ties at equal specificity break on sheet ORDER, so the old
    // erase+push_back silently promoted a re-added sheet to highest
    // priority among equals — permanently, and differently per session
    // depending on add order (C-10).
    auto it = std::find_if(m_GlobalStylesheets.begin(), m_GlobalStylesheets.end(),
                           [&](const StylesheetHandle& h)
                           { return h.get() == sheet.get(); });
    if (it != m_GlobalStylesheets.end())
        return;

    m_GlobalStylesheets.push_back(sheet);
    ++m_StylesheetSetGeneration;
    InvalidateAllInternedSheetSets();
}

void UIManager::ReplaceGlobalStylesheetBlock(const std::vector<const Stylesheet*>& oldBlock,
                                             const std::vector<StylesheetHandle>& newBlock)
{
    // Stage allocations before releasing any sheet referenced by the live caches.
    auto replacement = m_GlobalStylesheets;

    // Determine insertion point: the earliest index occupied by any old stylesheet.
    std::unordered_set<const Stylesheet*> oldSet;
    oldSet.reserve(oldBlock.size());
    for (const Stylesheet* s : oldBlock)
    {
        if (s)
            oldSet.insert(s);
    }

    size_t insertAt = replacement.size();
    if (!oldSet.empty())
    {
        for (size_t i = 0; i < replacement.size(); ++i)
        {
            const Stylesheet* ptr = replacement[i] ? replacement[i].get() : nullptr;
            if (ptr && oldSet.find(ptr) != oldSet.end())
            {
                insertAt = std::min(insertAt, i);
            }
        }
    }

    // Remove old block entries.
    if (!oldSet.empty())
    {
        auto it = std::remove_if(replacement.begin(), replacement.end(),
                                 [&](const StylesheetHandle& h)
                                 {
                                     const Stylesheet* ptr = h ? h.get() : nullptr;
                                     return ptr && oldSet.find(ptr) != oldSet.end();
                                 });
        replacement.erase(it, replacement.end());
    }

    if (insertAt > replacement.size())
        insertAt = replacement.size();

    // Prepare insertion list: dedupe within the new block AND against
    // survivors already in the list outside the removed block. Hot-reload
    // blocks include transitively @imported sheets whose surviving entries
    // (owned by another attached style) stay in place — re-inserting them
    // would duplicate every rule at a shifted cascade position (C-10).
    std::unordered_set<const Stylesheet*> surviving;
    surviving.reserve(replacement.size());
    for (const auto& h : replacement)
    {
        if (h)
            surviving.insert(h.get());
    }

    std::vector<StylesheetHandle> toInsert;
    toInsert.reserve(newBlock.size());
    std::unordered_set<const Stylesheet*> seenNew;
    seenNew.reserve(newBlock.size());
    for (const auto& h : newBlock)
    {
        if (!h)
            continue;
        const Stylesheet* ptr = h.get();
        if (!ptr)
            continue;
        if (surviving.find(ptr) != surviving.end())
            continue;
        if (!seenNew.insert(ptr).second)
            continue;
        toInsert.push_back(h);
    }

    if (!toInsert.empty())
    {
        replacement.insert(replacement.begin() + (ptrdiff_t)insertAt, toInsert.begin(), toInsert.end());
    }

    m_GlobalStylesheets.swap(replacement);
    ++m_StylesheetSetGeneration;

    // Primary-flip prep: every RetainedYogaNode's cached
    // PersistentSheetSetId references interner entries whose sheet-pointer
    // lists may contain the now-removed old block members. The pointers
    // themselves might have been freed (StylesheetHandles going out of
    // scope). Clear the interner so the next BuildYogaRecursive
    // re-resolves from scratch.
    InvalidateAllInternedSheetSets();
}

void UIManager::InvalidateAllInternedSheetSets()
{
    // P4 (C-10): the rooted invalidation walk below cannot reach detached
    // subtrees (inactive dock tabs, pooled rows). Bump the epoch so their
    // caches — possibly holding pointers into freed rule storage — fail
    // the IsRuleCacheValid check on their next cascade instead.
    ++m_RuleCacheEpoch;
    if (m_RuleCacheEpoch == 0)
        ++m_RuleCacheEpoch;

    m_SheetSetInterner.Clear();
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    // Walk the visible tree (children + Mount portals) and zero each
    // element's interned sheet-set ID. Stage 7 step 8: PSSID lives on
    // UIElement::m_YogaState now, so the walk is the only way to reach
    // every retained node.
    std::function<void(UIElement*)> zeroPssid = [&](UIElement* el) {
        if (!el) return;
        if (el->m_YogaState)
        {
            el->m_YogaState->PersistentSheetSetId = 0;
            // Stage 7 step 7 option 2: a global sheet-set change means
            // every element's merged sheet list could shift. The
            // subtree-skip gate would otherwise serve a stale cascade
            // result based on the old sheets — clear the gate flag to
            // force a slow-path rebuild on the next visit.
            el->m_YogaState->CachedSubtreeValid = false;
        }
        for (const auto& ch : el->GetChildren())
            zeroPssid(ch.get());
        if (auto* m = dynamic_cast<Mount*>(el))
        {
            if (UIElement* tgt = m->GetTarget())
                zeroPssid(tgt);
        }
    };
    zeroPssid(m_Root.get());
#endif
    // Cascade-memoization Phase 4: any sheet-set change invalidates the
    // structural rule pool for every element. Walk the root subtree so
    // the next cascade rebuilds caches against the new sheet ordering /
    // rule contents (this path covers global stylesheet add/remove,
    // hot-reload content swaps, and ReplaceGlobalStylesheetBlock).
    if (m_Root)
        m_Root->InvalidateRuleCacheSubtree();
}

void UIManager::NotifyStylesheetContentChanged()
{
    ++m_StylesheetContentGeneration;
    // Changed stylesheet snapshots or directly edited file styles invalidate indices that
    // depend on selector/rule structure must be rebuilt.
    m_StylesheetRuleIndexCache.clear();
    m_StylesheetRuleIndexCacheGeneration = 0;

    // Primary-flip prep: SheetSetInterner entries hold const
    // StylesheetRuleIndex* pointers sourced from m_StylesheetRuleIndexCache.
    // Clearing that cache just freed them; entries holding the stale
    // pointers are use-after-free waiting to happen. Clear the interner
    // and zero PSSIDs so the next pre-cascade pass re-interns with
    // fresh indices.
    InvalidateAllInternedSheetSets();

    // Retained-mode caching:
    // Even when elements are marked StyleDirty, we keep multiple layers of
    // retained caches (Yoga styles, draw styles, etc.). A stylesheet content
    // change must conservatively invalidate those caches so hot-reloaded
    // custom properties and selectors are observed immediately. Walks the
    // visible tree (children + Mount portals); StyleDirty alone preserves
    // the prior behavior (the loop iterated retained nodes and called
    // MarkDirty(StyleDirty) on each owning element).
    std::function<void(UIElement*)> markStyleDirty = [&](UIElement* el) {
        if (!el) return;
        el->MarkDirty(UIElement::StyleDirty);
        for (const auto& ch : el->GetChildren())
            markStyleDirty(ch.get());
        if (auto* m = dynamic_cast<Mount*>(el))
        {
            if (UIElement* tgt = m->GetTarget())
                markStyleDirty(tgt);
        }
    };
    markStyleDirty(m_Root.get());
}

namespace
{
static void MarkDirtyRecursive(UIElement* el, unsigned flags)
{
    if (!el)
        return;
    el->MarkDirty(flags);
    for (const auto& ch : el->GetChildren())
    {
        MarkDirtyRecursive(ch.get(), flags);
    }
    // Mount targets are portal-like and are not part of the child list. They must still
    // participate in dirty propagation so global stylesheet reloads affect all visible UI.
    if (auto* m = dynamic_cast<Mount*>(el))
    {
        if (UIElement* tgt = m->GetTarget())
        {
            MarkDirtyRecursive(tgt, flags);
        }
    }
}
} // namespace

void UIManager::MarkStyleDirtySubtree(UIElement* subtreeRoot)
{
    if (!subtreeRoot)
        return;
    MarkDirtyRecursive(subtreeRoot, UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
}

void UIManager::MarkStyleDirtyAll()
{
    MarkStyleDirtySubtree(m_Root.get());
}

// THE one place m_Root is replaced, and every path that installs a tree goes through it:
// SetRoot, LoadLayout, LoadLayoutFromAsset, LoadLayoutFromFile. Replacing the root DESTROYS
// the tree that was there, which is a destruction decision the subscribers are owed, so
// assigning m_Root directly silently drops their last notification. If a new load path
// appears, it belongs here rather than beside here.
void UIManager::AdoptRoot(std::unique_ptr<UIElement> newRoot)
{
    if (m_Root)
        UI::DispatchDestructionDetach(m_Root.get());
    m_Root = std::move(newRoot);
}

void UIManager::SetRoot(std::unique_ptr<UIElement> root)
{
    // When replacing the root, clear any input state that may reference elements
    // from the previous tree to avoid stale pointers (hover/capture ids). Focus is
    // cleared after AdoptRoot: the old tree's destruction is what tells a focused
    // control FocusOut, and it can only find that control while focus still names it.
    m_Hovered = nullptr;
    m_HoveredInstanceId = 0;
    m_MouseCaptured = false;
    m_CaptureElement = nullptr;
    m_CaptureInstanceId = 0;
    m_CaptureId.clear();
    // A whole-tree replace is a structural change: consumers that trust the
    // generation to detect staleness (hover recovery, pointer/idle gates)
    // must observe it.
    NotifyTreeStructureChanged();
    // The old tree is about to be destroyed without per-element detach, so drop
    // its analysis-seed entries; the new root re-registers any sheet-bearing
    // elements via SetOwnerManager(this) below.
    m_LocalSheetElements.clear();
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    ResetRetainedYogaTree();
#endif
    // Drop any queued virtualization work. Work items are keyed by instanceId and are safe
    // to keep around, but there is no reason to run them against a brand new root.
    m_VirtualizationCoordinator.Clear();
    AdoptRoot(std::move(root));
    m_FocusId.clear();
    m_LastFocusIdNotified.clear();
    m_FocusNotifiedInstanceId = 0;
    if (m_Root)
    {
        m_Root->SetOwnerManager(this);

        // Mount targets are portal-like and not part of the normal child list.
        // Ensure the owner manager is propagated to mounted subtrees so:
        // - retained dirty notifications work, and
        // - cached paint-command hit testing (fast paths) can consider them.
        std::vector<UIElement*> stack;
        stack.reserve(256);
        stack.push_back(m_Root.get());
        while (!stack.empty())
        {
            UIElement* el = stack.back();
            stack.pop_back();
            if (!el)
                continue;
            for (const auto& ch : el->GetChildren())
                if (ch)
                    stack.push_back(ch.get());
            if (auto* m = dynamic_cast<Mount*>(el))
            {
                if (UIElement* tgt = m->GetTarget())
                {
                    tgt->SetOwnerManager(this);
                    stack.push_back(tgt);
                }
            }
        }
    }
}

void UIManager::EnqueueVirtualizationWork(UIElement* ctx,
                                          VirtualizationCoordinator::RunFn fn,
                                          VirtualizationCoordinator::Reason reason)
{
    if (!ctx || !fn)
        return;
    m_VirtualizationCoordinator.Enqueue(ctx->GetInstanceId(), fn, reason);
}

UIElement* UIManager::FindElementByInstanceId(uint64_t instanceId) const
{
    if (instanceId == 0)
        return nullptr;

    UIElement* root = m_Root.get();
    if (!root)
        return nullptr;

    // Slice E1: O(1) map lookup (maintained on owner-gain / detach /
    // destruction) + an O(depth) reachability filter. The map is
    // ownership-scoped but the old tree walk only ever found ATTACHED
    // elements — owned-but-detached subtrees (inactive dock tabs) must
    // stay unfindable or hover/bubble resolves would target invisible
    // elements.
    UIElement* found = nullptr;
    auto it = m_ElementsByInstanceId.find(instanceId);
    if (it != m_ElementsByInstanceId.end() && UIElementReachesRoot(it->second, root))
        found = it->second;

#ifdef _DEBUG
    // Instance lookups must stay O(1). Set GE_UI_VALIDATE_INSTANCE_INDEX=1 to
    // compare the index against a full tree walk while debugging the index.
    static const bool s_ValidateInstanceIndex = []() {
        const char* value = std::getenv("GE_UI_VALIDATE_INSTANCE_INDEX");
        return value && value[0] == '1';
    }();
    if (s_ValidateInstanceIndex)
    {
        UIElement* walked = nullptr;
        std::vector<UIElement*> stack;
        stack.reserve(256);
        stack.push_back(root);
        while (!stack.empty())
        {
            UIElement* el = stack.back();
            stack.pop_back();
            if (!el)
                continue;
            if (el->GetInstanceId() == instanceId)
            {
                walked = el;
                break;
            }
            for (const auto& ch : el->GetChildren())
                if (ch)
                    stack.push_back(ch.get());
            if (auto* m = dynamic_cast<Mount*>(el))
            {
                if (UIElement* tgt = m->GetTarget())
                    stack.push_back(tgt);
            }
        }
        assert(walked == found &&
               "instanceId map diverged from the tree walk (E1 rollout check)");
    }
#endif

    return found;
}

void UIManager::ClearHover()
{
    UIElement* prev = m_Hovered;
    m_Hovered = nullptr;
    m_HoveredInstanceId = 0;
    m_MouseCaptured = false;
    m_CaptureElement = nullptr;
    m_CaptureInstanceId = 0;
    m_CaptureId.clear();

    // ResolveInPlace: called outside the frame's cascade (asset reload,
    // panel teardown) — re-bake now so the un-hovered style applies without
    // waiting for the next heavy pass.
    MarkHoverTransitionDirty(prev, nullptr, PseudoMarkMode::ResolveInPlace);
}

void UIManager::ClearHoverForSubtree(UIElement* subtreeRoot)
{
    if (!subtreeRoot || !m_Hovered)
        return;
    // Walk up from the currently hovered element; if we encounter the
    // subtree root, the hover lies within that subtree and should be cleared.
    UIElement* cur = m_Hovered;
    int depth = 0;
    constexpr int kMaxDepth = 256;
    while (cur && depth < kMaxDepth)
    {
        if (cur == subtreeRoot)
        {
            // Clear hover only — do NOT disturb mouse capture here.
            // ClearHoverForSubtree is called from local operations like
            // ListView::UnbindCell unbinding a pooled cell, while the
            // captured element (e.g. the scrollbar being dragged) typically
            // lives OUTSIDE the subtree. Calling the full ClearHover() would
            // wipe m_MouseCaptured/m_CaptureElement and silently terminate an
            // active drag the moment the cursor moves over a recycled cell.
            UIElement* prev = m_Hovered;
            m_Hovered = nullptr;
            m_HoveredInstanceId = 0;
            MarkHoverTransitionDirty(prev, nullptr, PseudoMarkMode::ResolveInPlace);
            break;
        }
        cur = cur->GetParent();
        ++depth;
    }
}

bool UIManager::AcceptTextureSpaceForFormat(const std::string& name,
                                            Rendering::TextureFormat format,
                                            UI::UITextureSpace space)
{
    const UI::UITextureSpaceCheck check = UI::CheckTextureSpaceAgainstFormat(format, space);
    if (check == UI::UITextureSpaceCheck::Valid)
        return true;

    // REFUSAL POLICY. A refused registration is a missing texture, which is a
    // changed frame — so only DisplayLinearSdr on a narrow format is refused:
    // its producers (#767 P1b) render into wide formats by construction, so
    // the contradiction can only be a new, wrong stamp, and refusing it keeps
    // the failure a loudly missing texture instead of a silently wrong colour.
    //
    // Every other contradiction warns and registers: refusing a stamp a
    // producer actually utters would blank real UI, which is worse than the
    // colour being wrong while the warning points at the producer to fix.
    const bool refuse = check == UI::UITextureSpaceCheck::ContradictsFormat &&
                        space == UI::UITextureSpace::DisplayLinearSdr();

#if defined(GE_DEV_DIAG)
    // Once per name: these registrations repeat every frame, and a per-frame
    // line would drown the log the diagnostic is meant to be visible in.
    if (m_TextureSpaceMismatchReported.insert(name).second)
    {
        if (check == UI::UITextureSpaceCheck::FormatNotClassifiable)
        {
            Logger::Log::Warning(
                "[UI TextureSpace] '{}' stamped {} on format {}, which carries no colour-space "
                "meaning — the stamp cannot be cross-checked",
                name, UI::ToString(space), static_cast<uint32_t>(format));
        }
        else
        {
            Logger::Log::Warning("[UI TextureSpace] '{}' stamped {} on format {} — the format "
                                 "cannot hold that space; registration {}",
                                 name, UI::ToString(space), static_cast<uint32_t>(format),
                                 refuse ? "REFUSED" : "allowed (#767 P1b corrects the stamp)");
        }
    }
#endif

    if (refuse)
    {
        // Loudly missing, never silently wrong: drop any prior binding and ask
        // the producer for this name again.
        RemoveExternalTexture(name);
        NoteUnresolvedExternalTextureRequest(name);
    }
    return !refuse;
}

void UIManager::SetExternalTexture(const std::string& name, Rendering::TextureHandle handle,
                                   Rendering::SamplerHandle sampler,
                                   uint32_t width, uint32_t height,
                                   UI::UITextureSpace space)
{
    if (!handle.IsValid())
    {
        RemoveExternalTexture(name);
        return;
    }

    // Live device query — the physical image's own format, not a value the
    // caller could restate wrongly alongside the stamp.
    const Rendering::TextureFormat format =
        m_Device ? m_Device->GetTextureFormat(handle) : Rendering::TextureFormat::Unknown;
    if (!AcceptTextureSpaceForFormat(name, format, space))
        return;

    auto it = m_ExternalDeviceTextures.find(name);
    if (it != m_ExternalDeviceTextures.end())
    {
        if (it->second.Handle == handle && it->second.Sampler == sampler &&
            it->second.Width == width && it->second.Height == height &&
            it->second.Space == space)
            return; // no change

        if (m_SdfTextureRegistry && it->second.Handle.IsValid() && it->second.Handle != handle)
            m_SdfTextureRegistry->UnregisterByHandle(it->second.Handle);

        it->second.Handle = handle;
        it->second.Sampler = sampler;
        it->second.Width = width;
        it->second.Height = height;
        it->second.Space = space;
    }
    else
    {
        m_ExternalDeviceTextures.insert_or_assign(
            name, ExternalDeviceTexture{handle, sampler, width, height, space});
    }
    // Remove from the RenderGraph map if present (device texture takes precedence).
    if (m_ExternalRGRegistrations.erase(name) > 0)
        m_ExternalRGPublished.erase(name);

    // Elements that referenced this key before it resolved have cached
    // primitives with no texture slot; force a regen so they pick it up.
    MarkPrimitivesNeedRegen(0x200u);
}

void UIManager::SetExternalTexture(const std::string& name, Rendering::TextureHandle handle,
                                   uint32_t width, uint32_t height,
                                   UI::UITextureSpace space)
{
    SetExternalTexture(name, handle, Rendering::SamplerHandle{}, width, height, space);
}

void UIManager::SetExternalTextureRG(const std::string& name, uint32_t width, uint32_t height,
                                      UI::UITextureSpace space, Rendering::TextureFormat format)
{
    if (!AcceptTextureSpaceForFormat(name, format, space))
        return;

    // RenderGraph registration is authoritative for this key: clear the direct
    // device binding mode (same hygiene as the device overloads).
    bool hadOtherMode = false;
    if (auto itDev = m_ExternalDeviceTextures.find(name); itDev != m_ExternalDeviceTextures.end())
    {
        if (m_SdfTextureRegistry && itDev->second.Handle.IsValid())
            m_SdfTextureRegistry->UnregisterByHandle(itDev->second.Handle);
        m_ExternalDeviceTextures.erase(itDev);
        hadOtherMode = true;
    }

    auto it = m_ExternalRGRegistrations.find(name);
    if (!hadOtherMode && it != m_ExternalRGRegistrations.end() &&
        it->second.Width == width && it->second.Height == height && it->second.Space == space)
        return; // no change

    m_ExternalRGRegistrations.insert_or_assign(name, ExternalRGRegistration{width, height, space});
    MarkPrimitivesNeedRegen(0x200u);
}

void UIManager::PublishExternalTextureRG(const std::string& name,
                                          Rendering::RenderGraph::RGFrame& frame,
                                          Rendering::RenderGraph::RGTexture tex)
{
    // Per-frame value: overwritten every frame the producer declares, and
    // validity-stamped so RenderRG ignores anything from another frame or a
    // stale incarnation of this one. Main-thread only (declaration scope).
    auto& pub = m_ExternalRGPublished[name];
    pub.TexId = tex.Id;
    pub.For.Stamp(frame);
}

void UIManager::NoteUnresolvedExternalTextureRequest(const std::string& name)
{
    if (!name.empty())
        m_UnresolvedExternalTextureRequests.insert(name);
}

std::vector<std::string> UIManager::ConsumeUnresolvedExternalTextureRequests()
{
    std::vector<std::string> out(m_UnresolvedExternalTextureRequests.begin(),
                                 m_UnresolvedExternalTextureRequests.end());
    m_UnresolvedExternalTextureRequests.clear();
    return out;
}

void UIManager::RemoveExternalTexture(const std::string& name)
{
    bool removed = false;
    auto it2 = m_ExternalDeviceTextures.find(name);
    if (it2 != m_ExternalDeviceTextures.end() && m_SdfTextureRegistry && it2->second.Handle.IsValid())
        m_SdfTextureRegistry->UnregisterByHandle(it2->second.Handle);
    if (it2 != m_ExternalDeviceTextures.end())
    {
        m_ExternalDeviceTextures.erase(it2);
        removed = true;
    }
    if (m_ExternalRGRegistrations.erase(name) > 0)
        removed = true;
    m_ExternalRGPublished.erase(name);
    if (removed)
        MarkPrimitivesNeedRegen(0x200u);
    static const bool s_Log = []() -> bool
    {
        const char* e = std::getenv("GE_UI_GEOM_INVALIDATE_LOG");
        return (e && e[0] == '1');
    }();
    static int s_Budget = 24;
    if (s_Log && s_Budget-- > 0)
    {
        Logger::Log::Warning("[UI GeoInvalidate] RemoveExternalTexture('{}')", name);
    }
}

void UIManager::ClearElementBackgroundTexture(UIElement& el)
{
    el.Styles().ResetBackgroundImage();

    static const bool s_Log = []() -> bool
    {
        const char* e = std::getenv("GE_UI_GEOM_INVALIDATE_LOG");
        return (e && e[0] == '1');
    }();
    static int s_Budget = 24;
    if (s_Log && s_Budget-- > 0)
    {
        Logger::Log::Warning("[UI GeoInvalidate] ClearElementBackgroundTexture(tag='{}' id='{}')",
                             UIAttributeAccess::GetDebugTypeName(el),
                             el.GetId().empty() ? "<no-id>" : el.GetId().c_str());
    }
}

void UIManager::SetFontResolver(FontResolverFn&& resolver)
{
    m_FontResolver = std::move(resolver);
    // Bump generation so any in-flight completions from the old resolver are ignored.
    ++m_FontResolverGeneration;
    // Clear request state; loaded atlases remain valid.
    m_FontFamilyInFlight.clear();
    m_FontFamilyRetryAfter.clear();
    m_FontFamilyFailCount.clear();
    m_FontAtlasAliases.clear();
}

void UIManager::RequestFontFamily(const std::string& family)
{
    (void)GetOrRequestFontFamilyInternal(family,
                                         /*weight=*/400,
                                         FontStyle::Normal,
                                         FontVariant::Normal);
}

UIElement* UIManager::GetRootElement() const
{
    return m_Root.get();
}

Rendering::Text::FontAtlas* UIManager::ResolveFontForStyle(const ResolvedStyle& style)
{
    if (style.Visual.FontFamily && !style.Visual.FontFamily->empty())
    {
        for (const auto& family : *style.Visual.FontFamily)
        {
            if (auto* fa = GetOrRequestFontFamilyInternal(family, style.Visual.FontWeight, style.Visual.FontStyle, style.Visual.FontVariant))
                return fa;
        }
    }

    // Fall back to the default atlas when available.
    if (m_FontAtlas)
        return m_FontAtlas.get();

    // As a last resort, reuse any family atlas we have.
    if (!m_FontAtlases.empty())
        return m_FontAtlases.begin()->second.get();

    return nullptr;
}

void UIManager::DrainDeferredActionsOnce()
{
    if (m_Dispatcher)
    {
        m_Dispatcher->Drain();
    }
}

GUID UIManager::ResolveBackgroundImagePath(const std::string& path,
                                           const std::string& sourceAlias)
{
    if (path.empty() || !m_AssetManager)
        return GUID::Null();

    std::string norm = UIUtil::NormalizeCssUrlPath(path);
    if (norm.empty())
        return GUID::Null();

    // Source-aware resolution: when CSS specified `url("editor:path")` or
    // `url("@editor/path")`, look up under exactly that source (no implicit
    // priority fallback). Let AssetManager register the file as part of
    // resolving the GUID; this covers first-run editor assets whose registry
    // entries do not exist yet immediately after a fresh build.
    GUID g = sourceAlias.empty()
                 ? m_AssetManager->ResolveAssetGuid(norm)
                 : m_AssetManager->ResolveAssetGuid(norm, sourceAlias);

    if (!g.IsNull())
    {
        m_BgPathToGuid[MakeBackgroundPathCacheKey(path, sourceAlias)] = g;
        if (norm != path)
            m_BgPathToGuid[MakeBackgroundPathCacheKey(norm, sourceAlias)] = g;
    }

    return g;
}

void UIManager::EvictBackgroundTexture(const std::string& path)
{
    if (path.empty())
        return;

    // Resolve to GUID via the path-to-guid cache.
    std::string norm = UIUtil::NormalizeCssUrlPath(path);
    GUID guid = GUID::Null();

    auto it = m_BgPathToGuid.find(path);
    if (it != m_BgPathToGuid.end())
        guid = it->second;
    else if (norm != path)
    {
        auto it2 = m_BgPathToGuid.find(norm);
        if (it2 != m_BgPathToGuid.end())
            guid = it2->second;
    }

    // Remove cached entries so the next load starts fresh.
    m_BgPathToGuid.erase(path);
    if (norm != path)
        m_BgPathToGuid.erase(norm);

    if (!guid.IsNull())
        EvictBackgroundTexture(guid);
}

void UIManager::EvictBackgroundTexture(const GUID& guid)
{
    if (guid.IsNull())
        return;

    auto itCache = m_BgTextureCache.find(guid);
    if (itCache == m_BgTextureCache.end())
        return;

    Rendering::TextureHandle oldHandle = itCache->second.Handle;
    const bool sharedOwned = itCache->second.SharedOwned;

    // E7: drop the old slot in the SDF texture registry. Its dedup key is the
    // raw handle id; once we destroy the underlying GPU texture the slot would
    // otherwise reference a freed handle and CreateTransientFrameSet would
    // bind it to descriptors next frame.
    if (m_SdfTextureRegistry && oldHandle.IsValid())
        m_SdfTextureRegistry->UnregisterByHandle(oldHandle);

    if (m_Device && oldHandle.IsValid() && !sharedOwned)
        m_Device->DestroyTexture(oldHandle);

    m_BgTextureCache.erase(itCache);

    // Drop any in-flight upload state so a subsequent EnsureBackgroundTextureUploaded
    // can re-trigger an async load.
    m_BgUploadInFlight.erase(guid);
}

void UIManager::EvictStaleBackgroundTextures()
{
    // The cache retains one GPU texture per distinct background/thumbnail ever
    // painted, so a long asset-browsing session grows it without bound. Once
    // it exceeds the byte budget, entries idle past the grace window drop
    // oldest-first. Current draw-order textures are protected even when their
    // primitives were retained without regenerating. The budget is soft when
    // the visible working set alone exceeds it.
    ++m_BgCacheFrame;

    constexpr uint64_t kSweepIntervalFrames = 120;
    constexpr uint64_t kBudgetBytes = 256ull * 1024 * 1024;
    // Nothing painted this recently is evictable, whatever the budget says —
    // it is likely still on screen (retained primitives don't re-touch).
    constexpr uint64_t kMinIdleFrames = 600;

    if ((m_BgCacheFrame % kSweepIntervalFrames) != 0)
        return;

    const auto entryBytes = [](const CachedTexture& e) -> uint64_t
    { return static_cast<uint64_t>(e.Width) * e.Height * 4u; };

    uint64_t totalBytes = 0;
    for (const auto& kv : m_BgTextureCache)
    {
        if (kv.second.Handle.IsValid() && !kv.second.SharedOwned)
            totalBytes += entryBytes(kv.second);
    }
    if (totalBytes <= kBudgetBytes)
        return;

    std::unordered_set<uint64_t> visibleHandles;
    if (m_SdfTextureRegistry)
    {
        std::vector<uint32_t> slots;
        slots.reserve(m_DrawOrder.size());
        for (const auto index : m_DrawOrder)
            if (index < m_PersistentPrimitives.size())
                slots.push_back(m_PersistentPrimitives[index].TextureIndex);
        for (const auto handle : m_SdfTextureRegistry->GetTextureHandlesForSlots(slots))
            visibleHandles.insert(static_cast<uint64_t>(handle.id));
    }

    struct Candidate
    {
        GUID Guid;
        uint64_t LastUsed = 0;
        uint64_t Bytes = 0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(m_BgTextureCache.size());
    for (auto& kv : m_BgTextureCache)
    {
        if (!kv.second.Handle.IsValid() || kv.second.SharedOwned)
            continue;
        if (visibleHandles.contains(static_cast<uint64_t>(kv.second.Handle.id)))
        {
            kv.second.LastUsedFrame = m_BgCacheFrame;
            continue;
        }
        if (kv.second.LastUsedFrame + kMinIdleFrames > m_BgCacheFrame)
            continue;
        candidates.push_back({kv.first, kv.second.LastUsedFrame, entryBytes(kv.second)});
    }
    if (candidates.empty())
        return;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.LastUsed < b.LastUsed; });

    size_t evictedCount = 0;
    uint64_t evictedBytes = 0;
    for (const Candidate& c : candidates)
    {
        if (totalBytes <= kBudgetBytes)
            break;
        EvictBackgroundTexture(c.Guid);
        totalBytes -= std::min(totalBytes, c.Bytes);
        evictedBytes += c.Bytes;
        ++evictedCount;
    }

    if (evictedCount > 0 && m_Root)
    {
        // Mirror the async-upload completion path: a style-dirty pass alone can
        // skip regen when CSS is unchanged, leaving retained primitives bound to
        // the dummy slot until some unrelated layout refresh. Force a primitive
        // rebuild so still-visible tiles re-resolve immediately.
        MarkPrimitivesNeedRegen(0x200u);
        MarkStyleDirtyAll();
        Logger::Log::Debug("[UI] Background texture cache over budget: evicted {} entries ({} KB)",
                           evictedCount, evictedBytes / 1024);
    }
}

void UIManager::EnsureBackgroundTextureUploaded(const GUID& guid)
{
    if (guid.IsNull() || !m_AssetManager || !m_Device)
        return;

    auto itCache = m_BgTextureCache.find(guid);
    if (itCache != m_BgTextureCache.end() && itCache->second.Handle.IsValid())
    {
        itCache->second.LastUsedFrame = m_BgCacheFrame;
        return;
    }

    // Try synchronous path: asset already loaded → upload GPU texture directly.
    auto asset = m_AssetManager->GetAsset(guid);
    if (asset)
    {
        auto* texA = dynamic_cast<TextureAsset*>(asset.get());
        if (texA)
        {
            uint32_t w = texA->GetWidth();
            uint32_t h = texA->GetHeight();
            if (w > 0 && h > 0)
            {
                Rendering::TextureHandle handle = CreateUIBackgroundTextureFromAsset(m_Device, texA);
                if (handle.IsValid())
                {
                    auto& entry = m_BgTextureCache[guid];
                    entry.Handle = handle;
                    entry.Width = w;
                    entry.Height = h;
                    entry.SharedOwned = false;
                    entry.Slice = ResolveNineSlice(guid);
                    entry.LastUsedFrame = m_BgCacheFrame;
                    return;
                }
            }
        }
    }

    // Asset not loaded yet — trigger async load. RequestBackgroundTexture
    // dedupes via m_BgUploadInFlight so repeated calls are cheap.
    RequestBackgroundTexture(guid, nullptr);
}

NineSlice UIManager::ResolveNineSlice(const GUID& guid)
{
    NineSlice slice;
    if (guid.IsNull() || !m_AssetManager)
        return slice;

    auto asset = m_AssetManager->GetAsset(guid);
    if (!asset)
        return slice;

    std::string value;
    if (m_AssetManager->GetRegistry().TryGetMetaValue(asset->GetPath(), kTextureNineSliceMetaKey, value))
        ParseTextureNineSliceMeta(value, slice);
    return slice;
}

uint32_t UIManager::TryRegisterResolvedBackgroundTexture(const std::string& path,
                                                        const std::string& sourceAlias,
                                                        UI::UITextureRegistry& reg,
                                                        uint32_t* outWidth,
                                                        uint32_t* outHeight,
                                                        Rendering::SamplerHandle sampler)
{
    if (outWidth)
        *outWidth = 0;
    if (outHeight)
        *outHeight = 0;

    const GUID g = ResolveBackgroundImagePath(path, sourceAlias);
    if (g.IsNull())
        return 0;

    EnsureBackgroundTextureUploaded(g);

    auto it = m_BgTextureCache.find(g);
    if (it == m_BgTextureCache.end() || !it->second.Handle.IsValid())
        return 0;

    if (outWidth)
        *outWidth = it->second.Width;
    if (outHeight)
        *outHeight = it->second.Height;

    return reg.Register(it->second.Handle, sampler);
}

Rendering::TextureHandle UIManager::TryGetBackgroundTextureHandleByGuid(const GUID& guid,
                                                                        uint32_t* outWidth,
                                                                        uint32_t* outHeight)
{
    if (outWidth)  *outWidth = 0;
    if (outHeight) *outHeight = 0;
    if (guid.IsNull())
        return Rendering::INVALID_TEXTURE_HANDLE;

    EnsureBackgroundTextureUploaded(guid);

    auto it = m_BgTextureCache.find(guid);
    if (it == m_BgTextureCache.end() || !it->second.Handle.IsValid())
        return Rendering::INVALID_TEXTURE_HANDLE;

    if (outWidth)  *outWidth  = it->second.Width;
    if (outHeight) *outHeight = it->second.Height;
    return it->second.Handle;
}

Rendering::TextureHandle UIManager::TryGetBackgroundTextureHandle(const std::string& path,
                                                                    uint32_t* outWidth,
                                                                    uint32_t* outHeight) const
{
    if (outWidth)  *outWidth = 0;
    if (outHeight) *outHeight = 0;

    // Cast away const — resolution and upload are logically read-only (lazy cache population).
    auto* self = const_cast<UIManager*>(this);

    const GUID g = self->ResolveBackgroundImagePath(path, "");
    if (g.IsNull())
        return Rendering::INVALID_TEXTURE_HANDLE;

    // Trigger upload if not yet cached (mirrors TryRegisterResolvedBackgroundTexture).
    self->EnsureBackgroundTextureUploaded(g);

    auto it = m_BgTextureCache.find(g);
    if (it == m_BgTextureCache.end() || !it->second.Handle.IsValid())
        return Rendering::INVALID_TEXTURE_HANDLE;

    if (outWidth)  *outWidth  = it->second.Width;
    if (outHeight) *outHeight = it->second.Height;
    return it->second.Handle;
}

void UIManager::RequestBackgroundTexture(const GUID& guid,
                                         std::function<void(Rendering::TextureHandle, uint32_t, uint32_t)> onReady)
{
    // Async background loads can outlive a tool window. The load completes on a worker, which
    // posts through the shared dispatcher: once this manager is gone the closed dispatcher
    // refuses the post, and posted work runs only from this manager's own drains.
    std::shared_ptr<UI::UiDispatcher> dispatcher = m_Dispatcher;

    if (guid.IsNull() || !m_AssetManager)
    {
        if (onReady)
            onReady(Rendering::TextureHandle{}, 0, 0);
        return;
    }

    // If already uploaded, invoke immediately
    auto it = m_BgTextureCache.find(guid);
    if (it != m_BgTextureCache.end() && it->second.Handle)
    {
        it->second.LastUsedFrame = m_BgCacheFrame;
        if (onReady)
            onReady(it->second.Handle, it->second.Width, it->second.Height);
        return;
    }

    // Queue callback for when ready
    if (onReady)
    {
        m_BgReadyCallbacks[guid].push_back(std::move(onReady));
    }

    // Already in flight — unless the request that set the marker never came
    // back. A completion can be lost (a dispatcher that stops draining, a load
    // that never resolves), and the marker outlives it, so every later attempt
    // returns here and the image never appears. Past the stale window the
    // request is re-issued rather than assumed to still be coming.
    static constexpr auto kUploadStale = std::chrono::seconds(5);
    static constexpr uint32_t kMaxUploadAttempts = 3;
    const auto now = std::chrono::steady_clock::now();
    auto itFlight = m_BgUploadInFlight.find(guid);
    if (itFlight != m_BgUploadInFlight.end())
    {
        if (now - itFlight->second.Issued < kUploadStale)
            return;
        if (itFlight->second.Attempts >= kMaxUploadAttempts)
        {
            if (itFlight->second.Attempts == kMaxUploadAttempts)
            {
                ++itFlight->second.Attempts; // log once, not every paint
                Logger::Log::Warning(
                    "UI: background texture {} never completed in {} attempts; it stays blank",
                    guid.ToString(), kMaxUploadAttempts);
            }
            return;
        }
        itFlight->second.Issued = now;
        ++itFlight->second.Attempts;
    }
    else
    {
        m_BgUploadInFlight.emplace(guid, BackgroundUploadRequest{now, 1});
    }

    auto completeLocal = [this, guid](Rendering::TextureHandle handle, uint32_t w, uint32_t h)
    {
        auto& entry = m_BgTextureCache[guid];
        if (handle && entry.Handle.IsValid() && entry.Handle != handle)
        {
            // A retried request can land after the one it replaced finally
            // arrives. Keep the texture already in the cache — primitives may
            // reference its slot — and free the late duplicate rather than
            // overwriting the handle and leaking it.
            if (m_Device && !entry.SharedOwned)
                m_Device->DestroyTexture(handle);
            handle = entry.Handle;
        }
        if (handle)
        {
            entry.Handle = handle;
            entry.Width = w;
            entry.Height = h;
            // Per-UIManager ownership: background textures are local to the manager.
            entry.SharedOwned = false;
            entry.Slice = ResolveNineSlice(guid);
            entry.LastUsedFrame = m_BgCacheFrame;
        }
        static const bool s_Log = []() -> bool
        {
            const char* e = std::getenv("GE_UI_GEOM_INVALIDATE_LOG");
            return (e && e[0] == '1');
        }();
        static int s_Budget = 24;
        if (s_Log && s_Budget-- > 0)
        {
            Logger::Log::Warning("[UI GeoInvalidate] BackgroundTextureReady(guid={})", guid.ToString());
        }

        m_BgUploadInFlight.erase(guid);
        if (!handle)
        {
            // The request completed with nothing to draw — the asset is not a
            // texture, or it decoded to zero. The element stays blank, and
            // nothing further is coming, so say so once.
            Logger::Log::Warning("UI: background image {} completed with no texture; it stays blank",
                                 guid.ToString());
        }
        auto itcb = m_BgReadyCallbacks.find(guid);
        if (itcb != m_BgReadyCallbacks.end())
        {
            for (auto& cb : itcb->second)
                cb(entry.Handle, entry.Width, entry.Height);
            m_BgReadyCallbacks.erase(itcb);
        }

        // Icon/background images often trigger this path with no readiness callback
        // (ResolveBackgroundTextureSlot calls RequestBackgroundTexture(guid, nullptr)).
        // A style-dirty refresh alone can be ignored by elements that have no layout
        // yet, leaving stale primitives until some unrelated layout refresh happens.
        // Force the primitive snapshot to rebuild as soon as the texture is available.
        if (handle)
        {
            MarkPrimitivesNeedRegen(0x200u);
            MarkStyleDirtyAll();
        }
    };

    auto runUploadFromAsset = [this, guid, completeLocal]()
    {
        if (!m_AppliesAsyncResults)
            return;
        Rendering::TextureHandle handle{};
        uint32_t w = 0, h = 0;
        auto a2 = m_AssetManager ? m_AssetManager->GetAsset(guid) : SharedPtr<Asset>{};
        if (a2)
        {
            if (auto* texA = dynamic_cast<TextureAsset*>(a2.get()))
            {
                w = texA->GetWidth();
                h = texA->GetHeight();
                if (w && h)
                {
                    handle = UIManager::CreateUIBackgroundTextureFromAsset(m_Device, texA);
                }
            }
        }
        completeLocal(handle, w, h);
    };

    // If already loaded in AssetManager, schedule the upload now via deferred UI action
    if (auto asset = m_AssetManager->GetAsset(guid))
    {
        if (dynamic_cast<TextureAsset*>(asset.get()))
        {
            dispatcher->Post(runUploadFromAsset);
        }
        else
        {
            // Not a texture asset; complete callbacks with failure on UI thread
            dispatcher->Post([this, completeLocal]()
                             {
                                 if (!m_AppliesAsyncResults)
                                     return;
                                 completeLocal(Rendering::TextureHandle{}, 0, 0); });
        }
        return;
    }

    // Submit async load and fan-out on completion
    m_AssetManager->LoadAsset(guid, [dispatcher, runUploadFromAsset](Result<SharedPtr<Asset>, AssetError>)
                              {
                                  // Switch to UI thread via UI dispatcher
                                  dispatcher->Post(runUploadFromAsset); });
}

std::optional<UIManager::ResolvedExternalTexture>
UIManager::ResolveExternalTexture(const std::string& name) const
{
    // GE_UI_REBIND_DIAG=1: trace which registry a thumbnail name resolves
    // through (device / RenderGraph registration) — the discriminator for "element
    // has the name but renders nothing" bugs.
    static const bool sResolveDiag = []() {
        const char* e = std::getenv("GE_UI_REBIND_DIAG");
        return e && e[0] == '1';
    }();
    const bool diagThis = sResolveDiag && name.rfind("editor_model_thumb", 0) == 0;
    auto diag = [&](const char* branch, uint32_t width, uint32_t height)
    {
        static int sBudget = 60;
        if (diagThis && sBudget-- > 0)
            Logger::Log::Info("[UI ResolveDiag] '{}' -> {} (w={} h={})", name, branch, width,
                              height);
    };

    {
        auto itDev = m_ExternalDeviceTextures.find(name);
        if (itDev != m_ExternalDeviceTextures.end() && itDev->second.Handle.IsValid())
        {
            diag("device", itDev->second.Width, itDev->second.Height);
            return ResolvedExternalTexture{itDev->second.Handle, itDev->second.Sampler,
                                           itDev->second.Width,  itDev->second.Height,
                                           itDev->second.Space,  /*Rg2Registered=*/false};
        }
    }

    {
        auto itRG = m_ExternalRGRegistrations.find(name);
        if (itRG != m_ExternalRGRegistrations.end())
        {
            diag("rg2-registration", itRG->second.Width, itRG->second.Height);
            return ResolvedExternalTexture{
                Rendering::TextureHandle{}, Rendering::SamplerHandle{}, itRG->second.Width,
                itRG->second.Height,        itRG->second.Space,         /*Rg2Registered=*/true};
        }
    }

    diag("UNRESOLVED", 0, 0);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Test-only accessors. These let UIHotReloadTests assert on internal cache
// state without exposing the maps publicly to non-test consumers.
// ---------------------------------------------------------------------------
bool UIManager::HasBackgroundTextureCachedForTesting(const GUID& guid) const
{
    auto it = m_BgTextureCache.find(guid);
    return it != m_BgTextureCache.end() && it->second.Handle.IsValid();
}

void UIManager::SeedBackgroundTextureForTesting(const GUID& guid,
                                                 Rendering::TextureHandle handle,
                                                 uint32_t width,
                                                 uint32_t height)
{
    if (guid.IsNull())
        return;
    auto& entry = m_BgTextureCache[guid];
    entry.Handle = handle;
    entry.Width = width;
    entry.Height = height;
    // Mark as shared-owned so eviction and ~UIManager don't call DestroyTexture
    // on a synthetic handle.
    entry.SharedOwned = true;
}

void UIManager::SeedFontGuidForTesting(const GUID& fontGuid, const std::string& atlasKey)
{
    if (fontGuid.IsNull() || atlasKey.empty())
        return;
    m_AtlasKeyToFontGuid[atlasKey] = fontGuid;
    auto& keys = m_FontGuidToAtlasKeys[fontGuid];
    if (std::find(keys.begin(), keys.end(), atlasKey) == keys.end())
        keys.push_back(atlasKey);
    // Insert a placeholder atlas pointer so HasFontAtlasKeyForTesting reflects
    // the seeded key. We deliberately avoid creating a real FontAtlas (would
    // require font bytes); the alias map is sufficient for eviction tests.
    m_FontAtlasAliases[atlasKey] = nullptr;
}

bool UIManager::HasFontAtlasKeyForTesting(const std::string& atlasKey) const
{
    if (m_FontAtlases.find(atlasKey) != m_FontAtlases.end())
        return true;
    if (m_FontAtlasAliases.find(atlasKey) != m_FontAtlasAliases.end())
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Asset hot-reload integration (C1 / C2 / E7)
//
// Subscribe to AssetEventDispatcher and forward Texture / Font events into
// per-UIManager pending lists. The dispatcher fires on the AssetManager's
// hot-reload thread; we keep the callback strictly queue-only so all GPU /
// UIElement mutation runs from DrainPendingAssetReloads() on the UI thread,
// driven by Update().
// ---------------------------------------------------------------------------
void UIManager::InstallAssetReloadCallback()
{
    if (!m_AssetManager || m_AssetEventCallbackHandle != 0)
        return;

    m_AssetEventCallbackHandle = m_AssetManager->GetEventDispatcher().AddCallback(
        [this](const AssetEvent& e)
        {
            if (e.Type != AssetType::Texture && e.Type != AssetType::Font)
                return;
            // Both Reloaded (asset-pipeline-driven) and Modified (file-watcher-only,
            // common for assets that aren't loaded as Asset objects, e.g. Font) trigger
            // a refresh. Created texture events also wake unresolved CSS background
            // paths so first-run editor icons can appear after a fresh build/register.
            const bool isReloadEvent =
                (e.EventType == AssetEventType::AssetReloaded ||
                 e.EventType == AssetEventType::AssetModified);
            const bool isTextureCreated =
                (e.Type == AssetType::Texture && e.EventType == AssetEventType::AssetCreated);
            if (!isReloadEvent && !isTextureCreated)
                return;

            std::lock_guard<std::mutex> lk(m_AssetReloadMutex);
            if (isTextureCreated)
                m_PendingTextureCatalogDirty = true;
            else if (e.Type == AssetType::Texture)
                m_PendingTextureReloads.push_back(e.AssetGuid);
            else
                m_PendingFontReloads.push_back(e.AssetGuid);
        });
}

void UIManager::UninstallAssetReloadCallback()
{
    if (!m_AssetManager || m_AssetEventCallbackHandle == 0)
        return;
    m_AssetManager->GetEventDispatcher().RemoveCallback(m_AssetEventCallbackHandle);
    m_AssetEventCallbackHandle = 0;
}

void UIManager::DrainPendingAssetReloads()
{
    std::vector<GUID> textures;
    std::vector<GUID> fonts;
    bool textureCatalogDirty = false;
    {
        std::lock_guard<std::mutex> lk(m_AssetReloadMutex);
        textures.swap(m_PendingTextureReloads);
        fonts.swap(m_PendingFontReloads);
        textureCatalogDirty = m_PendingTextureCatalogDirty;
        m_PendingTextureCatalogDirty = false;
    }

    auto dedupe = [](std::vector<GUID>& v)
    {
        if (v.size() <= 1)
            return;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    dedupe(textures);
    dedupe(fonts);

    for (const GUID& g : textures)
        HandleTextureAssetReloaded(g);
    for (const GUID& g : fonts)
        HandleFontAssetReloaded(g);
    if (textureCatalogDirty && m_Root)
        MarkStyleDirtyAll();
}

void UIManager::HandleTextureAssetReloaded(const GUID& guid)
{
    if (guid.IsNull())
        return;
    auto itCache = m_BgTextureCache.find(guid);
    if (itCache == m_BgTextureCache.end())
        return; // Not a background image we've cached; nothing to do.

    EvictBackgroundTexture(guid);

    // Mark every element that may reference this background as dirty so the
    // next paint re-resolves the URL → fresh handle. The conservative-but-cheap
    // approach is a tree-wide style/visual invalidation: hot reloads are rare
    // and the cost is bounded by the live element count. Walking the tree to
    // pick exactly the elements whose ResolvedStyle.Visual.BackgroundImage
    // resolves to this GUID would also miss elements where the source kind is
    // Path (lazy-resolved through m_BgPathToGuid) and whose resolved guid was
    // not yet cached on the element.
    if (m_Root)
        MarkStyleDirtyAll();
}

void UIManager::HandleFontAssetReloaded(const GUID& guid)
{
    if (guid.IsNull())
        return;

    // The default atlas (m_FontAtlas) is owned outside m_FontAtlases /
    // m_FontAtlasAliases. If this GUID resolved to it, drop it so the next
    // text frame rebuilds via the resolver. Without this, an early default
    // atlas with stale bytes would survive even after every aliased entry
    // is erased below.
    if (m_DefaultFontAtlasGuid == guid)
    {
        m_FontAtlas.reset();
        m_DefaultFontAtlasGuid = {};
    }

    auto itKeys = m_FontGuidToAtlasKeys.find(guid);
    if (itKeys == m_FontGuidToAtlasKeys.end())
    {
        // Default atlas was the only consumer (or no consumer). Still mark
        // text dirty so reflow happens with the freshly resolved atlas.
        if (m_Root)
            MarkStyleDirtyAll();
        m_FontResolvedDirty = true;
        return;
    }

    // Drop affected atlas / alias entries. FontAtlas::LoadFontBytes regenerates
    // m_AtlasId from scratch on the next bytes load, so the registered Slug /
    // color-page entries in UITextureRegistry (keyed by atlasId) become stale
    // and must be discarded. We tear down the FontAtlas instance entirely; the
    // next text frame will GetOrRequestFontFamilyInternal -> resolver and
    // rebuild a fresh atlas with new bytes.
    //
    // The key list includes both alias-consumer keys AND the owning key
    // for shared atlases (recorded in OnFontResolved), so erasing
    // m_FontAtlases[key] actually frees the underlying atlas even when
    // multiple keys aliased the same instance.
    for (const std::string& key : itKeys->second)
    {
        m_FontAtlases.erase(key);
        m_FontAtlasAliases.erase(key);
        m_AtlasKeyToFontGuid.erase(key);
        // Reset retry/in-flight bookkeeping so the resolver fires immediately.
        m_FontFamilyInFlight.erase(key);
        m_FontFamilyRetryAfter.erase(key);
        m_FontFamilyFailCount.erase(key);
    }
    m_FontGuidToAtlasKeys.erase(itKeys);

    // Mark text geometry dirty: visual + style are conservative but ensure
    // any per-frame measurement caches are invalidated and reflow happens.
    if (m_Root)
        MarkStyleDirtyAll();

    m_FontResolvedDirty = true;
}
