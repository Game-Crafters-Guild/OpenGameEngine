#include "EditorApplication.h"
#include "EditorPanelIds.h"
#include "Panels/AssetViewPanel.h"
#include "UI/Layout/Docking.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "Engine/Rendering/RenderDeviceContext.h"

namespace GameEngine
{

namespace
{

size_t FindTabIndex(const DockNode* leaf, const std::string& panelId)
{
    if (!leaf || !leaf->IsLeaf())
        return 0;

    const auto& tabs = leaf->GetTabs();
    for (size_t index = 0; index < tabs.size(); ++index)
    {
        if (tabs[index] == panelId)
            return index;
    }

    return tabs.size();
}

} // namespace

AssetViewPanel* EditorApplication::FindAssetViewPanelById(DockingManager* docking, const char* panelId)
{
    return docking ? dynamic_cast<AssetViewPanel*>(docking->GetPanel(panelId)) : nullptr;
}

void EditorApplication::ShowOrFocusAssetViewInSceneTabs(EditorWindowContext* ctx)
{
    if (!m_Docking || !ctx || !ctx->ui)
        return;

    std::string copyLeafPath;
    if (m_Docking->FindLeafContaining(EditorPanelIds::AssetViewSceneCopy, copyLeafPath))
    {
        (void)m_Docking->RemoveTab(EditorPanelIds::AssetViewSceneCopy);
        (void)m_Docking->ActivateTab(EditorPanelIds::SceneView);
    }
    else
    {
        std::string sceneLeafPath;
        const DockNode* sceneLeaf = m_Docking->FindLeafContaining(EditorPanelIds::SceneView, sceneLeafPath);
        if (!sceneLeaf)
            return;

        AssetViewPanel* sceneAssetViewPanel = EditorApplication::FindAssetViewPanelById(m_Docking.get(), EditorPanelIds::AssetViewSceneCopy);
        if (!sceneAssetViewPanel)
        {
            auto panel = std::make_unique<AssetViewPanel>();
            sceneAssetViewPanel = panel.get();
            if (m_EditorContext)
                sceneAssetViewPanel->SetContext(m_EditorContext.get());
            sceneAssetViewPanel->SetDevice(ctx->renderCtx ? ctx->renderCtx->GetDevice() : nullptr);
            sceneAssetViewPanel->SetUIManager(ctx->ui.get());
            sceneAssetViewPanel->SetAssetPreview(m_CurrentAssetPreviewPath, m_CurrentAssetPreviewEnabled);
            m_PanelStorage.emplace_back(std::move(panel));
            m_Docking->RegisterPanel(EditorPanelIds::AssetViewSceneCopy, sceneAssetViewPanel);
        }

        (void)m_Docking->DockAsTabInLeafByPath(sceneLeafPath, EditorPanelIds::AssetViewSceneCopy);

        std::string currentLeafPath;
        const DockNode* currentLeaf = m_Docking->FindLeafContaining(EditorPanelIds::AssetViewSceneCopy, currentLeafPath);
        if (currentLeaf)
        {
            const size_t sceneTabIndex = FindTabIndex(currentLeaf, EditorPanelIds::SceneView);
            const size_t copyTabIndex = FindTabIndex(currentLeaf, EditorPanelIds::AssetViewSceneCopy);
            const size_t desiredIndex = std::min(sceneTabIndex + 1, currentLeaf->GetTabs().size());
            (void)m_Docking->ReorderTabInLeafByPath(currentLeafPath, copyTabIndex, desiredIndex);
        }

        (void)m_Docking->ActivateTab(EditorPanelIds::AssetViewSceneCopy);
    }

    if (UIElement* rootEl = ctx->ui->GetRootElement())
    {
        if (auto* el = rootEl->FindById("dock"))
        {
            if (auto* dockspace = dynamic_cast<DockspaceElement*>(el))
            {
                dockspace->BindModel(m_Docking.get());
                dockspace->RequestRebuildFromModel();
                rootEl->PostAction([this, rootEl, ctx]()
                {
                    rootEl->PostAction([this, ctx]()
                    {
                        if (AssetViewPanel* sceneAssetViewPanel =
                                EditorApplication::FindAssetViewPanelById(m_Docking.get(), EditorPanelIds::AssetViewSceneCopy))
                        {
                            if (m_EditorContext)
                                sceneAssetViewPanel->SetContext(m_EditorContext.get());
                            sceneAssetViewPanel->SetDevice(ctx->renderCtx ? ctx->renderCtx->GetDevice() : nullptr);
                            sceneAssetViewPanel->SetUIManager(ctx->ui.get());
                            sceneAssetViewPanel->SetAssetPreview(m_CurrentAssetPreviewPath, m_CurrentAssetPreviewEnabled);
                        }
                    });
                });
            }
        }
    }
}

} // namespace GameEngine
