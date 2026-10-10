#include "EZTreeStatsPanel.h"

#include "Components/Name.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/World.h"
#include "EZTree/EZTreeOptions.h"
#include "EZTreeECS/Components/EZTreeComponent.h"
#include "UI/Controls/Label.h"
#include "UI/UIManager.h"

// Template implementations for Query/GetComponent instantiation inside a
// module TU — these require ECS/World.h to be included first.
#include "ECS/ECSTemplates.h"
#include "ECS/WorldTemplateImplementations.inl"

#include <memory>
#include <string>
#include <vector>

namespace GameEngine::EZTreeEditor
{
namespace
{

struct TreeRow
{
    std::string Name;
    std::string Detail;
    bool Enabled = true;
};

std::unique_ptr<Label> MakeRowLabel(const std::string& text, const char* className)
{
    auto label = std::make_unique<Label>();
    label->SetText(text);
    label->AddClass(className);
    return label;
}

} // namespace

EZTreeStatsPanel::EZTreeStatsPanel()
    : AssetBoundDockPanel("Tree Stats", "eztree", "Editor/UI/panels/EZTreeStatsPanel.uxml",
                          "Editor/UI/panels/EZTreeStatsPanel.css")
{
    AddClass("eztree-stats-panel");
}

EZTreeStatsPanel::~EZTreeStatsPanel()
{
    if (UIManager* manager = m_RefreshManager.Get(); manager && m_RefreshToken != 0)
        manager->UnregisterPeriodicRefresh(m_RefreshToken);
}

void EZTreeStatsPanel::OnPostLayout()
{
    AssetBoundDockPanel::OnPostLayout(); // deferred first-show asset bind
}

void EZTreeStatsPanel::ResolveElements()
{
    m_SummaryLabel = MakeWeakRef(dynamic_cast<Label*>(FindById("EZTreeStatsSummary")));
    m_RowsContainer = MakeWeakRef(FindById("EZTreeStatsRows"));
}

void EZTreeStatsPanel::OnLayoutReconciled()
{
    ResolveElements();
    // The summary label or rows container may be new, empty elements: rebuild even
    // if the scene data did not change.
    m_LastSnapshot.clear();
    RefreshStats();
}

void EZTreeStatsPanel::OnLayoutBound()
{
    ResolveElements();
    RefreshStats();

    // Register once on the manager's low-frequency refresh tick so the panel
    // stays current even while the editor is input-idle — OnPostLayout only
    // fires on heavy passes. Snapshot diffing in RefreshStats keeps an
    // unchanged tick free of tree mutation, so a steady scene still idles.
    if (m_RefreshToken == 0)
    {
        if (UIManager* manager = GetOwnerManager())
        {
            m_RefreshManager = UIManagerRef(manager);
            m_RefreshToken = manager->RegisterPeriodicRefresh([this]() { RefreshStatsIfVisible(); });
        }
    }
}

void EZTreeStatsPanel::RefreshStatsIfVisible()
{
    if (!IsLayoutBound())
        return;
    if (GetLayoutWidth() <= 0.0f || GetLayoutHeight() <= 0.0f)
        return; // inactive tab: skip the scene walk entirely
    RefreshStats();
}

void EZTreeStatsPanel::RefreshStats()
{
    UIElement* const rowsContainer = m_RowsContainer.Get();
    if (!rowsContainer)
        return;

    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();

    std::vector<TreeRow> rows;
    uint64 totalVertices = 0;
    uint64 totalTriangles = 0;
    if (world)
    {
        // Switched-off trees stay listed, dimmed.
        auto trees = world->Query<ECS::Read<Components::EZTree>,
                                  ECS::Optional<ECS::ComponentDisabled<Components::EZTree>>>();
        trees.IncludeDisabled<Components::EZTree>();
        trees.Each(
            [&](ECS::EntityHandle entity, const Components::EZTree& tree,
                const ECS::ComponentDisabled<Components::EZTree>* treeOff)
            {
                TreeRow row;
                const auto* name = world->GetComponent<Components::Name>(entity);
                row.Name = (name && name->value[0] != '\0') ? name->View() : "Tree";
                row.Detail = EZTree::ToString(tree.Options.type) + " · seed " +
                             std::to_string(tree.Options.seed) + " · " +
                             std::to_string(tree.RuntimeVertexCount) + " verts · " +
                             std::to_string(tree.RuntimeTriangleCount) + " tris · " +
                             std::to_string(tree.RuntimeBranchCount) + " branches";
                row.Enabled = treeOff == nullptr;
                totalVertices += tree.RuntimeVertexCount;
                totalTriangles += tree.RuntimeTriangleCount;
                rows.push_back(std::move(row));
            });
    }

    std::string snapshot{'\2'}; // sentinel: never equals the default-empty member
    for (const TreeRow& row : rows)
        snapshot += row.Name + '\0' + row.Detail + (row.Enabled ? '\1' : '\0');
    if (snapshot == m_LastSnapshot)
        return;
    m_LastSnapshot = std::move(snapshot);

    if (Label* const summaryLabel = m_SummaryLabel.Get())
    {
        if (rows.empty())
            summaryLabel->SetText("No Tree Generator entities in the scene.");
        else
            summaryLabel->SetText(std::to_string(rows.size()) + " tree(s) · " +
                                  std::to_string(totalVertices) + " vertices · " +
                                  std::to_string(totalTriangles) + " triangles");
    }

    rowsContainer->RemoveAllChildren();
    for (const TreeRow& row : rows)
    {
        auto rowEl = std::make_unique<UIElement>();
        rowEl->AddClass("eztree-stats-row");
        if (!row.Enabled)
            rowEl->AddClass("disabled");
        rowEl->AddChild(MakeRowLabel(row.Name, "eztree-stats-row-name"));
        rowEl->AddChild(MakeRowLabel(row.Detail, "eztree-stats-row-detail"));
        rowsContainer->AddChild(std::move(rowEl));
    }
}

} // namespace GameEngine::EZTreeEditor
