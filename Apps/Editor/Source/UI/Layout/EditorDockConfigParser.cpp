#include "UI/Layout/EditorDockConfigParser.h"

#include "UI/UIElement.h"
#include "UI/Layout/DockConfigElements.h"

#include <algorithm>
#include <unordered_set>

namespace GameEngine
{
namespace EditorUI
{
namespace
{

static bool HasClass(const UIElement& el, const char* cls)
{
    if (!cls || !*cls)
        return false;
    const auto& classes = el.GetClasses();
    return std::find(classes.begin(), classes.end(), std::string(cls)) != classes.end();
}

static const UIElement* FindChildByClass(const UIElement& parent, const char* cls)
{
    for (const auto& c : parent.GetChildren())
    {
        if (!c)
            continue;
        if (HasClass(*c, cls))
            return c.get();
    }
    return nullptr;
}

static const UIElement* FindChildByClassAndId(const UIElement& parent, const char* cls, const char* id)
{
    for (const auto& c : parent.GetChildren())
    {
        if (!c)
            continue;
        if (!HasClass(*c, cls))
            continue;
        if (id && *id)
        {
            if (c->GetId() != id)
                continue;
        }
        return c.get();
    }
    return nullptr;
}

static bool ParseLeaf(const UIElement& el, const std::unordered_set<std::string>& declaredPanels, std::unique_ptr<DockNode>& out)
{
    const auto* leafEl = dynamic_cast<const DockLeafNodeElement*>(&el);
    if (!leafEl)
        return false;
    auto leaf = DockNode::MakeLeaf();

    // Collect tabs in authored order.
    for (const auto& c : el.GetChildren())
    {
        if (!c)
            continue;
        const auto* tabEl = dynamic_cast<const DockTabNodeElement*>(c.get());
        if (!tabEl)
            continue;
        const std::string& panelId = tabEl->GetPanelId();
        if (panelId.empty())
            return false;
        if (!declaredPanels.empty() && declaredPanels.find(panelId) == declaredPanels.end())
            return false;
        leaf->AddTab(panelId);
    }

    // Optional active="PanelId"
    const std::string& active = leafEl->GetActiveTab();
    if (!active.empty())
    {
        (void)leaf->ActivateTab(active);
    }

    out = std::move(leaf);
    return true;
}

static bool ParseNodeRecursive(const UIElement& el,
                               const std::unordered_set<std::string>& declaredPanels,
                               std::unique_ptr<DockNode>& out)
{
    if (HasClass(el, "dock-leaf"))
    {
        return ParseLeaf(el, declaredPanels, out);
    }

    if (HasClass(el, "dock-split"))
    {
        const auto* splitEl = dynamic_cast<const DockSplitElement*>(&el);
        if (!splitEl)
            return false;
        const DockPosition dir = splitEl->GetDirection();
        const float ratio = splitEl->GetRatio();

        // Expect exactly two logical children for a split; ignore any non-node metadata.
        std::vector<const UIElement*> nodeChildren;
        nodeChildren.reserve(2);
        for (const auto& c : el.GetChildren())
        {
            if (!c)
                continue;
            if (HasClass(*c, "dock-leaf") || HasClass(*c, "dock-split"))
            {
                nodeChildren.push_back(c.get());
            }
        }
        if (nodeChildren.size() != 2)
            return false;

        std::unique_ptr<DockNode> first;
        std::unique_ptr<DockNode> second;
        if (!ParseNodeRecursive(*nodeChildren[0], declaredPanels, first))
            return false;
        if (!ParseNodeRecursive(*nodeChildren[1], declaredPanels, second))
            return false;

        auto split = std::make_unique<DockNode>();
        split->SetSplit(dir, ratio, std::move(first), std::move(second));
        out = std::move(split);
        return true;
    }

    return false;
}

} // namespace

bool TryParseEditorDockConfigFromDockspace(UIElement* dockspace, EditorDockConfig& out)
{
    out = EditorDockConfig{};
    if (!dockspace)
        return false;

    // Find dock-config root under dockspace.
    const UIElement* cfg = FindChildByClassAndId(*dockspace, "dock-config", "dock-config");
    if (!cfg)
        cfg = FindChildByClass(*dockspace, "dock-config");
    if (!cfg)
        return false;

    // Parse panel inventory.
    // Supports either:
    // - direct children of DockConfig, or
    // - a DockPanels container (recommended) containing DockablePanel children.
    std::unordered_set<std::string> panelIds;
    std::vector<const UIElement*> panelNodes;
    panelNodes.reserve(16);

    for (const auto& c : cfg->GetChildren())
    {
        if (!c)
            continue;
        if (HasClass(*c, "dock-panel"))
        {
            panelNodes.push_back(c.get());
            continue;
        }
        if (HasClass(*c, "dock-panels"))
        {
            for (const auto& pc : c->GetChildren())
            {
                if (!pc)
                    continue;
                if (HasClass(*pc, "dock-panel"))
                {
                    panelNodes.push_back(pc.get());
                }
            }
        }
    }

    for (const UIElement* n : panelNodes)
    {
        if (!n)
            continue;

        EditorDockPanelDef def{};
        def.id = n->GetId();
        const auto* panelEl = dynamic_cast<const DockablePanelElement*>(n);
        if (!panelEl)
            return false;
        def.type = panelEl->GetPanelType();
        def.title = panelEl->GetPanelTitle();
        def.layout = panelEl->GetLayoutPath();
        def.style = panelEl->GetStylePath();
        def.icon = panelEl->GetTabIcon();
        def.showInMenu = panelEl->GetShowInMenu();

        if (def.id.empty() || def.type.empty())
            return false;
        if (!panelIds.insert(def.id).second)
            return false;

        out.panels.push_back(std::move(def));
    }
    if (out.panels.empty())
        return false;

    // Find the default layout node.
    const UIElement* layout = FindChildByClassAndId(*cfg, "dock-layout", "default");
    if (!layout)
        layout = FindChildByClass(*cfg, "dock-layout");
    if (!layout)
        return false;

    // A dock-layout should contain exactly one root node (leaf or split).
    const UIElement* layoutRoot = nullptr;
    for (const auto& c : layout->GetChildren())
    {
        if (!c)
            continue;
        if (HasClass(*c, "dock-leaf") || HasClass(*c, "dock-split"))
        {
            if (layoutRoot)
                return false;
            layoutRoot = c.get();
        }
    }
    if (!layoutRoot)
        return false;

    std::unique_ptr<DockNode> dockRoot;
    if (!ParseNodeRecursive(*layoutRoot, panelIds, dockRoot))
        return false;

    out.defaultLayout.root = std::move(dockRoot);
    return out.defaultLayout.root != nullptr;
}

} // namespace EditorUI
} // namespace GameEngine


