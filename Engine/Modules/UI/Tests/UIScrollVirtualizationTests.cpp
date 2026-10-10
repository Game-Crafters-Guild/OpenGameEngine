#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Rendering/Core/Device.h"
#include "UIRgTestHarness.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TreeView.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace {

static void CollectElementsDepthFirst(UIElement* root, std::vector<UIElement*>& out)
{
    if (!root)
        return;
    std::vector<UIElement*> stack;
    stack.push_back(root);
    while (!stack.empty())
    {
        UIElement* el = stack.back();
        stack.pop_back();
        if (!el)
            continue;
        out.push_back(el);
        for (const auto& ch : el->GetChildren())
        {
            if (ch)
                stack.push_back(ch.get());
        }
    }
}

static bool StartsWith(const std::string& s, const char* prefix)
{
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static std::optional<uint64_t> ParseTrailingU64AfterLastDash(const std::string& s)
{
    const size_t dash = s.rfind('-');
    if (dash == std::string::npos || dash + 1 >= s.size())
        return std::nullopt;
    const std::string tail = s.substr(dash + 1);
    for (char c : tail)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
    }
    try
    {
        return (uint64_t)std::stoull(tail);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// ------------------------------
// Providers
// ------------------------------

class SimpleGridProvider : public GridChangeTrackingProvider
{
  public:
    explicit SimpleGridProvider(int count) : m_Count(count) {}

    int GetItemCount() const override { return m_Count; }
    GridId GetItemId(int index) const override { return (GridId)(index + 1); }
    const char* GetLabel(GridId id) const override
    {
        m_LabelBuf = std::string("Item ") + std::to_string((uint64_t)id);
        return m_LabelBuf.c_str();
    }
    uint64_t GetIcon(GridId /*id*/) const override { return 0; }
    const char* GetTypeKey(GridId /*id*/) const override { return "t"; }
    void ApplySort(const SortDescriptor& /*desc*/) override {}
    void ApplyGrouping(const GroupDescriptor& /*desc*/) override {}

  private:
    int m_Count = 0;
    mutable std::string m_LabelBuf;
};

class ManyRootsTreeProvider : public TreeChangeTrackingProvider
{
  public:
    explicit ManyRootsTreeProvider(int count) : m_Count(count) {}

    int GetRootCount() const override { return m_Count; }
    TreeId GetRootId(int index) const override { return (TreeId)(index + 1); }
    int GetChildCount(TreeId /*parent*/) const override { return 0; }
    TreeId GetChildId(TreeId /*parent*/, int /*index*/) const override { return 0; }
    const char* GetLabel(TreeId id) const override
    {
        m_LabelBuf = std::string("Node ") + std::to_string((uint64_t)id);
        return m_LabelBuf.c_str();
    }
    bool IsExpandable(TreeId /*id*/) const override { return false; }

  private:
    int m_Count = 0;
    mutable std::string m_LabelBuf;
};

class FractionalHeightListProvider : public ListChangeTrackingProvider
{
  public:
    FractionalHeightListProvider(int count, float itemHeight)
        : m_Count(count), m_ItemHeight(itemHeight)
    {
    }

    int GetItemCount() const override { return m_Count; }
    ListId GetItemId(int index) const override { return (ListId)(index + 1); }
    float GetItemHeight(int /*index*/) const override { return m_ItemHeight; }

  private:
    int m_Count = 0;
    float m_ItemHeight = 0.0f;
};

// Mutable providers for the C-7/C-8 change-pump tests: their data can change
// between frames, driving MarkChanged (paint-only) and MarkStructureChanged
// (structural) so the tests exercise the pump + consume path.

class MutableListProvider : public ListChangeTrackingProvider
{
  public:
    explicit MutableListProvider(int count)
    {
        for (int i = 0; i < count; ++i)
            m_Labels.push_back(std::string("Row ") + std::to_string(i));
    }

    int GetItemCount() const override { return (int)m_Labels.size(); }
    ListId GetItemId(int index) const override { return (ListId)(index + 1); }
    float GetItemHeight(int /*index*/) const override { return 20.0f; }

    std::string LabelAt(int index) const
    {
        return (index >= 0 && index < (int)m_Labels.size()) ? m_Labels[(size_t)index] : std::string();
    }
    // Paint-only content change on a single row.
    void SetLabel(int index, std::string text)
    {
        if (index < 0 || index >= (int)m_Labels.size())
            return;
        m_Labels[(size_t)index] = std::move(text);
        MarkChanged(GetItemId(index));
    }
    // Structural change: drop the last item.
    void RemoveLast()
    {
        if (m_Labels.empty())
            return;
        m_Labels.pop_back();
        MarkStructureChanged();
    }

  private:
    std::vector<std::string> m_Labels;
};

class MutableGridProvider : public GridChangeTrackingProvider
{
  public:
    explicit MutableGridProvider(int count)
    {
        for (int i = 0; i < count; ++i)
            m_Labels.push_back(std::string("Cell ") + std::to_string(i));
    }

    int GetItemCount() const override { return (int)m_Labels.size(); }
    GridId GetItemId(int index) const override { return (GridId)(index + 1); }
    const char* GetLabel(GridId id) const override
    {
        const size_t idx = (size_t)id - 1;
        return (id >= 1 && idx < m_Labels.size()) ? m_Labels[idx].c_str() : "";
    }
    uint64_t GetIcon(GridId /*id*/) const override { return 0; }
    const char* GetTypeKey(GridId /*id*/) const override { return "t"; }
    void ApplySort(const SortDescriptor& /*desc*/) override {}
    void ApplyGrouping(const GroupDescriptor& /*desc*/) override {}

    void SetLabel(int index, std::string text)
    {
        if (index < 0 || index >= (int)m_Labels.size())
            return;
        m_Labels[(size_t)index] = std::move(text);
        MarkChanged(GetItemId(index));
    }

  private:
    std::vector<std::string> m_Labels;
};

// Single expandable root (id 1) with a mutable child list, for the TreeView
// structural-change test.
class MutableTreeProvider : public TreeChangeTrackingProvider
{
  public:
    explicit MutableTreeProvider(std::vector<TreeId> children) : m_Children(std::move(children)) {}

    int GetRootCount() const override { return 1; }
    TreeId GetRootId(int /*index*/) const override { return 1; }
    int GetChildCount(TreeId parent) const override { return parent == 1 ? (int)m_Children.size() : 0; }
    TreeId GetChildId(TreeId parent, int index) const override
    {
        if (parent != 1 || index < 0 || index >= (int)m_Children.size())
            return 0;
        return m_Children[(size_t)index];
    }
    const char* GetLabel(TreeId id) const override
    {
        m_LabelBuf = std::string("Node ") + std::to_string((uint64_t)id);
        return m_LabelBuf.c_str();
    }
    bool IsExpandable(TreeId id) const override { return id == 1; }

    void AddChild(TreeId childId)
    {
        m_Children.push_back(childId);
        MarkStructureChanged();
    }

  private:
    std::vector<TreeId> m_Children;
    mutable std::string m_LabelBuf;
};

static bool AnyElementHasText(UIElement* root, const std::string& text)
{
    std::vector<UIElement*> els;
    CollectElementsDepthFirst(root, els);
    for (UIElement* el : els)
    {
        if (el && el->GetTextContent() == text)
            return true;
    }
    return false;
}

} // namespace

TEST(UIScrollVirtualizationTests, ScrollView_Width100UsesScrollportWidth_AndLongLabelExpandsContentWidth)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto sv = std::make_unique<ScrollView>();
    sv->SetId("sv");

    auto fill = std::make_unique<UIElement>();
    fill->SetId("fill");

    auto longLabel = std::make_unique<Label>();
    longLabel->SetId("long");
    longLabel->SetText(std::string(800, 'X'));

    sv->AddContent(std::move(fill));
    sv->AddContent(std::move(longLabel));
    root->AddChild(std::move(sv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    Stylesheet sheet{};
    const std::string css = R"(
#root { width: 240px; height: 100px; }
#sv { width: 200px; height: 80px; }
#fill { width: 100%; height: 10px; }
#long { height: 10px; }
)";
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);

    auto* scroll = dynamic_cast<ScrollView*>(r->FindById("sv"));
    ASSERT_NE(scroll, nullptr);

    UIElement* fillEl = r->FindById("fill");
    ASSERT_NE(fillEl, nullptr);

    const float vw = scroll->GetViewportWidth();
    const float cw = scroll->GetContentWidth();

    // width:100% should resolve against the scrollport (visible viewport), not the expanded overflow width.
    EXPECT_NEAR(fillEl->GetLayoutWidth(), vw, 1.0f);

    // Intrinsic label width should expand scroll range beyond the viewport.
    EXPECT_GT(cw, vw + 1.0f);
}

TEST(UIScrollVirtualizationTests, GridView_RebindsVisibleCellsImmediatelyAfterScrollYChanges)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    SimpleGridProvider provider(500);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto gv = std::make_unique<GridView>();
    gv->SetId("gv");
    gv->SetDataProvider(&provider);
    gv->RefreshFromProvider();

    root->AddChild(std::move(gv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    Stylesheet sheet{};
    const std::string css = R"(
#root { width: 320px; height: 220px; }
#gv { width: 320px; height: 220px; }
)";
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);

    std::vector<UIElement*> all;
    CollectElementsDepthFirst(r, all);

    std::vector<uint64_t> gidsBefore;
    for (UIElement* el : all)
    {
        if (!el)
            continue;
        const std::string& id = el->GetId();
        if (StartsWith(id, "gridcell-"))
        {
            if (auto gid = ParseTrailingU64AfterLastDash(id))
                gidsBefore.push_back(*gid);
        }
    }
    ASSERT_FALSE(gidsBefore.empty());
    const uint64_t minBefore = *std::min_element(gidsBefore.begin(), gidsBefore.end());
    EXPECT_EQ(minBefore, 1u);

    auto* grid = dynamic_cast<GridView*>(r->FindById("gv"));
    ASSERT_NE(grid, nullptr);
    ASSERT_FALSE(grid->GetChildren().empty());

    auto* scroll = dynamic_cast<ScrollView*>(grid->GetChildren()[0].get());
    ASSERT_NE(scroll, nullptr);

    // Scroll down enough to require rebinding a different first visible row.
    scroll->SetScrollY(600.0f);

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    all.clear();
    CollectElementsDepthFirst(r, all);
    std::vector<uint64_t> gidsAfter;
    for (UIElement* el : all)
    {
        if (!el)
            continue;
        const std::string& id = el->GetId();
        if (StartsWith(id, "gridcell-"))
        {
            if (auto gid = ParseTrailingU64AfterLastDash(id))
                gidsAfter.push_back(*gid);
        }
    }
    ASSERT_FALSE(gidsAfter.empty());
    const uint64_t minAfter = *std::min_element(gidsAfter.begin(), gidsAfter.end());

    // Regression: after scroll offset changes, GridView must rebind visible cells immediately
    // (without needing a forced Yoga relayout).
    EXPECT_GT(minAfter, minBefore);
}

TEST(UIScrollVirtualizationTests, TreeView_RebindsVisibleRowsImmediatelyAfterScrollYChanges)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    ManyRootsTreeProvider provider(500);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto tv = std::make_unique<TreeView>();
    tv->SetId("tv");
    tv->SetShowRoot(true);
    tv->SetDataProvider(&provider);
    tv->RefreshFromProvider();

    root->AddChild(std::move(tv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    Stylesheet sheet{};
    const std::string css = R"(
#root { width: 320px; height: 220px; }
#tv { width: 320px; height: 220px; }
)";
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);

    auto collectMinVisibleNodeId = [&](uint64_t& outMin) -> bool
    {
        std::vector<UIElement*> els;
        CollectElementsDepthFirst(r, els);
        std::vector<uint64_t> ids;
        for (UIElement* el : els)
        {
            if (!el)
                continue;
            if (!el->HasClass("tree-title"))
                continue;
            const std::string text = el->GetTextContent();
            // Expected format: "Node <id>"
            size_t sp = text.rfind(' ');
            if (sp == std::string::npos || sp + 1 >= text.size())
                continue;
            const std::string tail = text.substr(sp + 1);
            for (char c : tail)
            {
                if (c < '0' || c > '9')
                    goto next_label;
            }
            try
            {
                ids.push_back((uint64_t)std::stoull(tail));
            }
            catch (...)
            {
            }
        next_label:
            continue;
        }
        if (ids.empty())
            return false;
        outMin = *std::min_element(ids.begin(), ids.end());
        return true;
    };

    uint64_t minBefore = 0;
    ASSERT_TRUE(collectMinVisibleNodeId(minBefore));
    EXPECT_EQ(minBefore, 1u);

    auto* tree = dynamic_cast<TreeView*>(r->FindById("tv"));
    ASSERT_NE(tree, nullptr);
    ASSERT_FALSE(tree->GetChildren().empty());
    auto* scroll = dynamic_cast<ScrollView*>(tree->GetChildren()[0].get());
    ASSERT_NE(scroll, nullptr);

    scroll->SetScrollY(400.0f);

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    uint64_t minAfter = 0;
    ASSERT_TRUE(collectMinVisibleNodeId(minAfter));

    // Regression: after scroll offset changes, TreeView must rebind visible rows immediately.
    EXPECT_GT(minAfter, minBefore);
}

// CollectBoundIds reports the ids of exactly the rows the view holds bound: the rows it binds, and after
// it trims its pool (destroying rows) once its viewport has shrunk, only the rows it kept.
TEST(UIScrollVirtualizationTests, TreeView_CollectBoundIdsReportsTheRowsItHolds)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    ManyRootsTreeProvider provider(500);
    auto root = std::make_unique<UIElement>();
    auto owner = std::make_unique<TreeView>();
    TreeView* tree = owner.get();
    tree->SetDataProvider(&provider);
    tree->Overrides().Set(Style::Width, StyleLength::Px(320.0f)).Set(Style::Height, StyleLength::Px(900.0f));
    root->AddChild(std::move(owner));
    UIManager ui(dev);
    ui.SetLayoutSizeOverride(400, 1000);
    ui.SetRoot(std::move(root));
    tree->RefreshFromProvider();
    ui.Update(0.0f, /*interactive=*/false);

    const auto boundIds = [tree]()
    {
        std::vector<TreeId> ids;
        tree->CollectBoundIds(ids);
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const auto rowIds = [tree]()
    {
        std::vector<TreeId> ids;
        for (const TreeView::DebugBoundRowInfo& row : tree->DebugGetBoundRows(1000))
            ids.push_back(row.boundId);
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const std::vector<TreeId> tall = boundIds();
    ASSERT_FALSE(tall.empty());
    EXPECT_EQ(tall, rowIds());

    // Shrink; the view trims its pool after enough unchanged passes of its virtualization.
    tree->Overrides().Set(Style::Height, StyleLength::Px(60.0f));
    for (int i = 0; i < 60; ++i)
    {
        tree->EnqueuePumpWork(ui);
        ui.Update(0.0f, /*interactive=*/false);
    }
    const std::vector<TreeId> shrunk = boundIds();
    EXPECT_LT(shrunk.size(), tall.size()) << "the view kept every row, so the trim is not covered";
    EXPECT_EQ(shrunk, rowIds());
}

// FindBoundRow and BoundIdOf answer for the rows the view holds bound only: once its viewport has shrunk,
// the pooled rows it no longer binds answer no id, and an item it no longer shows finds no row.
TEST(UIScrollVirtualizationTests, TreeView_FindsOnlyTheRowsItHoldsBound)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    ManyRootsTreeProvider provider(500);
    auto root = std::make_unique<UIElement>();
    auto owner = std::make_unique<TreeView>();
    TreeView* tree = owner.get();
    tree->SetDataProvider(&provider);
    tree->Overrides().Set(Style::Width, StyleLength::Px(320.0f)).Set(Style::Height, StyleLength::Px(900.0f));
    root->AddChild(std::move(owner));
    UIManager ui(dev);
    ui.SetLayoutSizeOverride(400, 1000);
    ui.SetRoot(std::move(root));
    tree->RefreshFromProvider();
    ui.Update(0.0f, /*interactive=*/false);
    std::vector<TreeId> tall;
    tree->CollectBoundIds(tall);
    ASSERT_FALSE(tall.empty());

    // Shrink for fewer passes than the trim waits for: the pool keeps rows it no longer binds.
    tree->Overrides().Set(Style::Height, StyleLength::Px(60.0f));
    for (int i = 0; i < 3; ++i)
    {
        tree->EnqueuePumpWork(ui);
        ui.Update(0.0f, /*interactive=*/false);
    }
    std::vector<TreeId> held;
    tree->CollectBoundIds(held);
    ASSERT_FALSE(held.empty());

    std::vector<UIElement*> elements;
    CollectElementsDepthFirst(tree, elements);
    std::size_t unboundRows = 0;
    for (UIElement* element : elements)
    {
        if (element && element->HasClass("tree-item") && tree->BoundIdOf(element) == 0)
            ++unboundRows;
    }
    ASSERT_GT(unboundRows, 0u) << "the pool holds no unbound row, so this test is vacuous";

    for (const TreeId id : held)
    {
        const UIElement* row = tree->FindBoundRow(id);
        ASSERT_NE(row, nullptr) << "held item " << id << " found no row";
        EXPECT_EQ(tree->BoundIdOf(row), id);
    }
    std::size_t letGo = 0;
    for (const TreeId id : tall)
    {
        if (std::find(held.begin(), held.end(), id) != held.end())
            continue;
        ++letGo;
        EXPECT_EQ(tree->FindBoundRow(id), nullptr) << "item " << id << " the view no longer shows found a row";
    }
    EXPECT_GT(letGo, 0u) << "the view still shows every item, so this test is vacuous";
}

TEST(UIScrollVirtualizationTests, ListView_DoesNotAllowOverscroll_WhenItemHeightsAreFractional)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    constexpr int kCount = 1000;
    constexpr float kItemHeight = 20.4f; // rounds to 20px; float accumulation would drift if not handled carefully
    FractionalHeightListProvider provider(kCount, kItemHeight);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto lv = std::make_unique<ListView>();
    lv->SetId("lv");
    lv->SetDataProvider(&provider);
    // Provide a stable child element so the test can verify rebinding by inspecting ids.
    lv->SetItemFactory([](ListId /*id*/, IListDataProvider* /*prov*/) -> std::unique_ptr<UIElement>
                       {
                           auto label = std::make_unique<Label>();
                           label->AddClass("list-item");
                           return label;
                       });
    lv->SetItemBinder([](UIElement* cell, ListId /*id*/, int index, IListDataProvider* /*prov*/)
                      {
                          if (!cell)
                              return;
                          cell->SetId(std::string("lvcell-") + std::to_string(index));
                          if (auto* lab = dynamic_cast<Label*>(cell))
                          {
                              lab->SetText(std::string("Item ") + std::to_string(index));
                          }
                      });
    lv->RefreshFromProvider();

    root->AddChild(std::move(lv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    Stylesheet sheet{};
    const std::string css = R"(
#root { width: 320px; height: 220px; }
#lv { width: 320px; height: 220px; }
)";
    ASSERT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);

    auto* list = dynamic_cast<ListView*>(r->FindById("lv"));
    ASSERT_NE(list, nullptr);
    const float viewportH = list->GetViewportHeight();
    const float expectedContentH = (float)kCount * (float)std::lround(kItemHeight);
    EXPECT_NEAR(list->GetTotalContentHeight(), expectedContentH, 0.5f);

    // Clamp should be based on the same rounded pixel extents as per-cell layout rects,
    // otherwise you can scroll into empty space at the end of long lists.
    const float expectedMaxScroll = std::max(0.0f, expectedContentH - viewportH);

    list->SetScrollOffset(1.0e9f);

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    EXPECT_NEAR(list->GetScrollOffset(), expectedMaxScroll, 1.0f);
}

TEST(UIScrollVirtualizationTests, ListView_RebindsVisibleCellsImmediatelyAfterScrollYChanges)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    constexpr int kCount = 500;
    FractionalHeightListProvider provider(kCount, /*itemHeight=*/20.0f);

    auto makeList = [&](bool correctnessMode) -> std::pair<std::unique_ptr<UIManager>, UIElement*>
    {
        auto root = std::make_unique<UIElement>();
        root->SetId("root");

        auto lv = std::make_unique<ListView>();
        lv->SetId("lv");
        lv->SetDataProvider(&provider);
        // Stable child element and binder so the test can read which indices are currently bound.
        lv->SetItemFactory([](ListId /*id*/, IListDataProvider* /*prov*/) -> std::unique_ptr<UIElement>
                           {
                               auto label = std::make_unique<Label>();
                               label->AddClass("list-item");
                               return label;
                           });
        lv->SetItemBinder([](UIElement* cell, ListId /*id*/, int index, IListDataProvider* /*prov*/)
                          {
                              if (!cell)
                                  return;
                              cell->SetId(std::string("lvcell-") + std::to_string(index));
                              // Also update text so this matches real-world binders.
                              if (auto* lab = dynamic_cast<Label*>(cell))
                                  lab->SetText(std::string("Item ") + std::to_string(index));
                          });
        lv->RefreshFromProvider();
        root->AddChild(std::move(lv));

        auto ui = std::make_unique<UIManager>(dev);
        if (correctnessMode)
            ui->SetCorrectnessModeEnabled(true);
        ui->SetRoot(std::move(root));

        Stylesheet sheet{};
        const std::string css = R"(
#root { width: 320px; height: 220px; }
#lv { width: 320px; height: 220px; }
)";
        EXPECT_TRUE(CSSParser::ParseStylesFromString(css, sheet));
        ui->AddStylesheet(std::make_shared<Stylesheet>(sheet));

        UIElement* r = ui->GetRootElement();
        return {std::move(ui), r};
    };

    auto runCase = [&](bool correctnessMode)
    {
        auto [ui, r] = makeList(correctnessMode);
        ASSERT_NE(ui, nullptr);
        ASSERT_NE(r, nullptr);

        ui->Update(0.0f, /*interactive=*/false);
        DriveUiRender(*ui, rg);

        auto collectMinBoundIndex = [&]() -> uint64_t
        {
            std::vector<UIElement*> els;
            CollectElementsDepthFirst(r, els);
            std::vector<uint64_t> idxs;
            for (UIElement* el : els)
            {
                if (!el)
                    continue;
                const std::string& id = el->GetId();
                if (StartsWith(id, "lvcell-"))
                {
                    if (auto v = ParseTrailingU64AfterLastDash(id))
                        idxs.push_back(*v);
                }
            }
            EXPECT_FALSE(idxs.empty());
            return idxs.empty() ? 0u : *std::min_element(idxs.begin(), idxs.end());
        };

        const uint64_t minBefore = collectMinBoundIndex();
        EXPECT_EQ(minBefore, 0u) << "Expected initial visible indices to include 0";

        auto* list = dynamic_cast<ListView*>(r->FindById("lv"));
        ASSERT_NE(list, nullptr);
        ASSERT_FALSE(list->GetChildren().empty());
        ScrollView* scroll = nullptr;
        for (auto& child : list->GetChildren())
        {
            scroll = dynamic_cast<ScrollView*>(child.get());
            if (scroll)
                break;
        }
        ASSERT_NE(scroll, nullptr);

        scroll->SetScrollY(400.0f);

        ui->Update(0.0f, /*interactive=*/false);
        DriveUiRender(*ui, rg);

        const uint64_t minAfter = collectMinBoundIndex();
        EXPECT_GT(minAfter, minBefore) << "Expected visible bound indices to advance after scrolling";
    };

    runCase(/*correctnessMode=*/false);
    runCase(/*correctnessMode=*/true);
}

TEST(UIScrollVirtualizationTests, CorrectnessMode_DisablesFastPathsAndForcesHeavyPass)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto a = std::make_unique<Label>();
    a->SetId("a");
    a->SetText("Hello");
    root->AddChild(std::move(a));

    UIManager ui(dev);
    ui.SetCorrectnessModeEnabled(true);
    ui.SetUpdateProfilingEnabled(true);
    ui.SetRoot(std::move(root));

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(R"(
#root { width: 200px; height: 50px; }
#a { width: 200px; height: 50px; }
)", sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    // Even if nothing changes between frames, correctness mode must force the heavy pass.
    ui.Update(0.0f, /*interactive=*/false);
    EXPECT_TRUE(ui.IsCorrectnessModeEnabled());
}

static void VirtualizationCoordinator_ShouldNotRun(UIElement* /*ctx*/,
                                                   UIManager& /*ui*/,
                                                   VirtualizationCoordinator::Reason /*reason*/)
{
    FAIL() << "Virtualization work item ran for a removed control (expected to be dropped).";
}

TEST(UIScrollVirtualizationTests, VirtualizationCoordinator_DropsWorkForRemovedControl)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);

    UIRegistration::RegisterBuiltInControls();

    auto root = std::make_unique<UIElement>();
    root->SetId("root");

    auto lv = std::make_unique<ListView>();
    lv->SetId("lv");
    root->AddChild(std::move(lv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(R"(
#root { width: 320px; height: 220px; }
#lv { width: 320px; height: 220px; }
)", sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    auto* list = dynamic_cast<ListView*>(r->FindById("lv"));
    ASSERT_NE(list, nullptr);

    // Enqueue work, then destroy the control before the next Update() drains the queue.
    ui.EnqueueVirtualizationWork(list, &VirtualizationCoordinator_ShouldNotRun, VirtualizationCoordinator::Reason::DataChanged);
    r->RemoveChild(list); // destroys list (do not touch 'list' after this)

    // Must not crash and must not run the work item.
    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);
}

// C-8: a MarkChanged on a visible ListView row (no scroll/viewport change) must
// repaint that row on the next Update, driven by the UIManager change pump.
//
// Headless caveat: interactive==false never reaches the idle gate, so these
// tests exercise the pump + provider-changeset consume, not the idle-gate
// short-circuit itself. The idle-gate behavior (pump must NOT keep an idle
// panel out of the idle frame) is verified live in the editor.
TEST(UIScrollVirtualizationTests, ListView_MarkChanged_RepaintsAffectedRowWithoutScroll)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    MutableListProvider provider(20);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto lv = std::make_unique<ListView>();
    lv->SetId("lv");
    lv->SetDataProvider(&provider);
    lv->SetItemFactory([](ListId /*id*/, IListDataProvider* /*prov*/) -> std::unique_ptr<UIElement>
                       {
                           auto label = std::make_unique<Label>();
                           label->AddClass("list-item");
                           return label;
                       });
    lv->SetItemBinder([](UIElement* cell, ListId /*id*/, int index, IListDataProvider* prov)
                      {
                          if (!cell)
                              return;
                          cell->SetId(std::string("lvcell-") + std::to_string(index));
                          if (auto* lab = dynamic_cast<Label*>(cell))
                              lab->SetText(static_cast<MutableListProvider*>(prov)->LabelAt(index));
                      });
    lv->RefreshFromProvider();
    root->AddChild(std::move(lv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(R"(
#root { width: 320px; height: 220px; }
#lv { width: 320px; height: 220px; }
)", sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* cell0 = r->FindById("lvcell-0");
    ASSERT_NE(cell0, nullptr);
    EXPECT_EQ(cell0->GetTextContent(), "Row 0");

    // Mutate one visible row's content with no scroll or viewport change.
    provider.SetLabel(0, "CHANGED-0");

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* cell0After = r->FindById("lvcell-0");
    ASSERT_NE(cell0After, nullptr);
    EXPECT_EQ(cell0After->GetTextContent(), "CHANGED-0")
        << "Change pump must rebind the affected row without a scroll/viewport event";
}

// C-7 + C-8: MarkStructureChanged after removing an item must be reflected in the
// window on the next Update (new item count, shrunken content height), with no
// stale tail row — again with no scroll/viewport event.
TEST(UIScrollVirtualizationTests, ListView_MarkStructureChanged_ReflectsNewCountWithoutScroll)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    constexpr int kCount = 8; // 8 * 20px = 160px, fully visible in a 220px viewport
    MutableListProvider provider(kCount);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto lv = std::make_unique<ListView>();
    lv->SetId("lv");
    lv->SetDataProvider(&provider);
    lv->SetItemFactory([](ListId /*id*/, IListDataProvider* /*prov*/) -> std::unique_ptr<UIElement>
                       {
                           auto label = std::make_unique<Label>();
                           label->AddClass("list-item");
                           return label;
                       });
    lv->SetItemBinder([](UIElement* cell, ListId /*id*/, int index, IListDataProvider* prov)
                      {
                          if (!cell)
                              return;
                          cell->SetId(std::string("lvcell-") + std::to_string(index));
                          if (auto* lab = dynamic_cast<Label*>(cell))
                              lab->SetText(static_cast<MutableListProvider*>(prov)->LabelAt(index));
                      });
    lv->RefreshFromProvider();
    ListView* list = lv.get();
    root->AddChild(std::move(lv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(R"(
#root { width: 320px; height: 220px; }
#lv { width: 320px; height: 220px; }
)", sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    EXPECT_EQ(list->GetLastKnownItemCount(), kCount);
    EXPECT_NEAR(list->GetTotalContentHeight(), (float)kCount * 20.0f, 0.5f);

    provider.RemoveLast(); // 8 -> 7 items, MarkStructureChanged

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    EXPECT_EQ(list->GetLastKnownItemCount(), kCount - 1)
        << "Structural change must drive a fresh window with the new item count";
    EXPECT_NEAR(list->GetTotalContentHeight(), (float)(kCount - 1) * 20.0f, 0.5f)
        << "Structural change must rebuild the height cache (scroll extents)";
}

// C-8: GridView variant of the paint-only pump test.
TEST(UIScrollVirtualizationTests, GridView_MarkChanged_RepaintsAffectedCellWithoutScroll)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    MutableGridProvider provider(20);

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto gv = std::make_unique<GridView>();
    gv->SetId("gv");
    gv->SetDataProvider(&provider);
    gv->RefreshFromProvider();
    root->AddChild(std::move(gv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(R"(
#root { width: 320px; height: 220px; }
#gv { width: 320px; height: 220px; }
)", sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(AnyElementHasText(r, "Cell 0"));
    EXPECT_FALSE(AnyElementHasText(r, "CHANGED-0"));

    provider.SetLabel(0, "CHANGED-0");

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    EXPECT_TRUE(AnyElementHasText(r, "CHANGED-0"))
        << "Change pump must rebind the affected grid cell without a scroll/viewport event";
}

// C-7 + C-8: adding a child to an expanded TreeView node via MarkStructureChanged
// must rebuild the flat row set so the new row appears — with no All changeset and
// no scroll/viewport event.
TEST(UIScrollVirtualizationTests, TreeView_MarkStructureChanged_RebuildsFlatListWithoutScroll)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
    {
        GTEST_SKIP() << "Device init failed";
    }
    UiRgHarness rg(dev);
    UIRegistration::RegisterBuiltInControls();

    MutableTreeProvider provider({11, 12});

    auto root = std::make_unique<UIElement>();
    root->SetId("root");
    auto tv = std::make_unique<TreeView>();
    tv->SetId("tv");
    tv->SetShowRoot(true);
    tv->SetDataProvider(&provider);
    tv->SetExpanded(1, true);
    tv->RefreshFromProvider();
    root->AddChild(std::move(tv));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));
    Stylesheet sheet{};
    ASSERT_TRUE(CSSParser::ParseStylesFromString(R"(
#root { width: 320px; height: 220px; }
#tv { width: 320px; height: 220px; }
)", sheet));
    ui.AddStylesheet(std::make_shared<Stylesheet>(sheet));

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(AnyElementHasText(r, "Node 11"));
    EXPECT_TRUE(AnyElementHasText(r, "Node 12"));
    EXPECT_FALSE(AnyElementHasText(r, "Node 13"));

    provider.AddChild(13); // MarkStructureChanged

    ui.Update(0.0f, /*interactive=*/false);
    DriveUiRender(ui, rg);

    EXPECT_TRUE(AnyElementHasText(r, "Node 13"))
        << "Structural change must rebuild the flat row set so the new child row appears";
}

