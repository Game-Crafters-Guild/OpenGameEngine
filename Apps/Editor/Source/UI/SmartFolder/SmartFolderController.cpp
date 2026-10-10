#include "UI/SmartFolder/SmartFolderController.h"

#include "UI/SmartFolder/SmartFolderFilterEvaluator.h"

#include <algorithm>

namespace GameEngine
{

void SmartFolderController::SetAssetsRoot(std::filesystem::path assetsRoot)
{
    if (m_AssetsRoot == assetsRoot)
        return;
    m_AssetsRoot = std::move(assetsRoot);
    InvalidateResults();
}

void SmartFolderController::InvalidateResults()
{
    m_ResultCache.clear();
    m_TransientResults.clear();
}

const std::vector<std::filesystem::path>* SmartFolderController::Evaluate(
    const std::string& smartFolderId,
    AssetRegistry* registry)
{
    const SmartFolder* folder = m_Manager.GetById(smartFolderId);
    if (!folder)
        return nullptr;

    if (folder->Filters.empty())
    {
        m_TransientResults.clear();
        return &m_TransientResults;
    }

    if (CanCacheResults(*folder))
    {
        auto cached = m_ResultCache.find(smartFolderId);
        if (cached != m_ResultCache.end())
            return &cached->second;

        SmartFolderFilterEvaluator evaluator;
        auto [inserted, wasInserted] = m_ResultCache.emplace(
            smartFolderId, evaluator.Evaluate(*folder, m_AssetsRoot, registry));
        (void)wasInserted;
        return &inserted->second;
    }

    SmartFolderFilterEvaluator evaluator;
    m_TransientResults = evaluator.Evaluate(*folder, m_AssetsRoot, registry);
    return &m_TransientResults;
}

void SmartFolderController::SetSelected(const std::string& smartFolderId)
{
    m_SelectedId = smartFolderId;
    if (!m_SelectedId.empty() && m_OnSelectionChanged)
        m_OnSelectionChanged(m_SelectedId, &m_Manager);
}

void SmartFolderController::ClearSelection()
{
    m_SelectedId.clear();
    if (m_OnSelectionChanged)
        m_OnSelectionChanged(std::string(), nullptr);
}

bool SmartFolderController::CanCacheResults(const SmartFolder& folder)
{
    return std::none_of(folder.Filters.begin(), folder.Filters.end(), [](const SmartFolderFilter& filter)
    {
        return filter.FilterType == SmartFolderFilter::Type::Tag ||
               filter.FilterType == SmartFolderFilter::Type::VCSStatus;
    });
}

} // namespace GameEngine
