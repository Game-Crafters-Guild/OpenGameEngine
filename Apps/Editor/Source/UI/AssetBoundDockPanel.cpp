#include "UI/AssetBoundDockPanel.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <utility>

namespace GameEngine::Editor
{

AssetBoundDockPanel::AssetBoundDockPanel(const std::string& title, std::string assetSourceAlias,
                                         std::filesystem::path layoutAssetPath,
                                         std::filesystem::path styleAssetPath)
    : DockPanel(title)
    , m_AssetSourceAlias(std::move(assetSourceAlias))
    , m_LayoutAssetPath(std::move(layoutAssetPath))
    , m_StyleAssetPath(std::move(styleAssetPath))
{
    // The bind itself reconciles the layout once, before OnLayoutBound has run; only
    // reconciles after a completed bind reach OnLayoutReconciled.
    RegisterEventHandler(kEventLayoutReconciled,
                         [this](UIEvent&)
                         {
                             if (m_BindApplied)
                                 OnLayoutReconciled();
                         });
}

AssetBoundDockPanel::~AssetBoundDockPanel()
{
    // The pending asset callback captures this panel; withdraw it before the panel
    // goes away (destroying an AssetLoadHandle does not).
    if (m_LoadHandle)
        m_LoadHandle->Cancel();
}

void AssetBoundDockPanel::OnPostLayout()
{
    if (m_BindApplied || m_BindPending || m_BindFailed)
        return;
    if (GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return; // inactive dock tab / hidden
    if (!GetOwnerManager())
        return;

    // Defer out of the layout pass; the bind mutates this subtree.
    m_BindPending = true;
    // A dropped action never runs, so it would never release the latch: release it here
    // instead and let the next laid-out frame try again.
    if (!this->PostAction([this]() { this->BindFromAssets(); }))
        m_BindPending = false;
}

void AssetBoundDockPanel::BindFromAssets()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        // Detached between scheduling and draining: transient, so release the latch and
        // let the next laid-out frame schedule a fresh attempt.
        m_BindPending = false;
        return;
    }

    if (m_LayoutAssetPath.empty())
    {
        AttachStyle();
        return;
    }

    auto& am = EngineCore::GetInstance().GetAssetManager();
    const GUID layoutGuid = am.ResolveAssetGuid(m_LayoutAssetPath, m_AssetSourceAlias);
    if (layoutGuid.IsNull())
        return MarkBindFailed("layout", m_LayoutAssetPath);

    // Loads are asynchronous: a blocking future.get() here would stall the UI thread for
    // the whole load, on the frame the panel first becomes visible.
    m_LoadHandle = std::make_unique<AssetLoadHandle>(am.LoadAsset(
        layoutGuid,
        [this, layoutGuid, post = GetPostHandle()](Result<SharedPtr<Asset>, AssetError> r)
        {
            // The load completes on a worker, which must not touch this panel: it posts
            // through the handle, and every latch mutation below runs on the UI thread.
            const bool loaded = r.IsOk() && r.Value() && r.Value()->GetType() == AssetType::UILayout;
            post.Post(
                [this, layoutGuid, loaded]()
                {
                    if (!loaded)
                        return MarkBindFailed("layout", m_LayoutAssetPath);
                    UIManager* ui2 = GetOwnerManager();
                    if (!ui2)
                    {
                        m_BindPending = false;
                        return;
                    }
                    auto asset = EngineCore::GetInstance().GetAssetManager().GetAsset(layoutGuid);
                    if (!asset || asset->GetType() != AssetType::UILayout ||
                        !ui2->BindLayoutToSubtreeChildrenFromAsset(
                            this, *static_cast<UILayoutAsset*>(asset.get())))
                        return MarkBindFailed("layout", m_LayoutAssetPath);
                    AttachStyle();
                });
        },
        AssetLoadPriority::High));
}

void AssetBoundDockPanel::AttachStyle()
{
    if (m_StyleAssetPath.empty())
        return FinishBind();

    auto& am = EngineCore::GetInstance().GetAssetManager();
    const GUID styleGuid = am.ResolveAssetGuid(m_StyleAssetPath, m_AssetSourceAlias);
    if (styleGuid.IsNull())
        return MarkBindFailed("stylesheet", m_StyleAssetPath);

    m_LoadHandle = std::make_unique<AssetLoadHandle>(am.LoadAsset(
        styleGuid,
        [this, styleGuid, post = GetPostHandle()](Result<SharedPtr<Asset>, AssetError> r)
        {
            const bool loaded = r.IsOk() && r.Value() && r.Value()->GetType() == AssetType::UIStyle;
            post.Post(
                [this, styleGuid, loaded]()
                {
                    if (!loaded)
                        return MarkBindFailed("stylesheet", m_StyleAssetPath);
                    UIManager* ui = GetOwnerManager();
                    if (!ui)
                    {
                        m_BindPending = false;
                        return;
                    }
                    auto asset = EngineCore::GetInstance().GetAssetManager().GetAsset(styleGuid);
                    if (!asset || asset->GetType() != AssetType::UIStyle ||
                        !ui->AttachStyleToSubtreeFromAsset(
                            this, *static_cast<UIStyleAsset*>(asset.get())))
                        return MarkBindFailed("stylesheet", m_StyleAssetPath);
                    FinishBind();
                });
        },
        AssetLoadPriority::High));
}

void AssetBoundDockPanel::FinishBind()
{
    m_BindApplied = true;
    m_BindPending = false;
    OnLayoutBound();
}

void AssetBoundDockPanel::MarkBindFailed(const char* what, const std::filesystem::path& path)
{
    // A missing asset is a packaging bug, not a transient: fail loudly once instead of
    // re-resolving (and re-logging) every layout pass.
    m_BindPending = false;
    m_BindFailed = true;
    Logger::Log::Error("AssetBoundDockPanel('{}'): {} '{}' did not resolve from source '{}'",
                       GetTitle(), what, path.string(), m_AssetSourceAlias);
}

} // namespace GameEngine::Editor
