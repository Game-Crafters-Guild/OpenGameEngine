#include <gtest/gtest.h>

#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "Input/KeyCodes.h"

#include <any>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

// Minimal synchronous search provider for testing
class TestSearchProvider : public ISearchProvider
{
public:
    void SetItems(std::vector<SearchResultItem> items) { m_Items = std::move(items); }

    void BeginSearch(const std::string& query, ResultSink sink) override
    {
        ++m_SearchCount;
        m_LastQuery = query;

        std::vector<SearchResultItem> results;
        for (const auto& item : m_Items)
        {
            if (query.empty() || item.Label.find(query) != std::string::npos)
                results.push_back(item);
        }
        sink(std::move(results), /*isComplete=*/true);
    }

    void CancelSearch() override { ++m_CancelCount; }

    std::string GetPlaceholderText() const override { return m_Placeholder; }
    void SetPlaceholder(const std::string& text) { m_Placeholder = text; }

    int m_SearchCount = 0;
    int m_CancelCount = 0;
    std::string m_LastQuery;

private:
    std::vector<SearchResultItem> m_Items;
    std::string m_Placeholder = "Test search...";
};

// Helper: dispatch a key event to the dialog
static void SendKey(SearchDialog& dialog, int keyCode)
{
    UIEvent e{};
    e.Id = kEventKeyDown;
    e.Key = keyCode;
    e.Target = &dialog;
    e.CurrentTarget = &dialog;
    dialog.OnEvent(e);
}

static UIElement* FindFirstWithClass(UIElement* element, const std::string& className)
{
    if (!element)
        return nullptr;
    if (element->HasClass(className))
        return element;
    for (const auto& child : element->GetChildren())
    {
        if (UIElement* match = FindFirstWithClass(child.get(), className))
            return match;
    }
    return nullptr;
}

// Helper: create a set of test items
static std::vector<SearchResultItem> MakeTestItems(int count)
{
    std::vector<SearchResultItem> items;
    for (int i = 0; i < count; ++i)
    {
        SearchResultItem item;
        item.Id = static_cast<SearchItemId>(i + 1);
        item.Label = "Item" + std::to_string(i);
        item.Detail = "path/to/Item" + std::to_string(i);
        item.TypeKey = "test";
        item.Icon = SearchIcon::FromClass("icon-test");
        item.UserData = i;
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace

// ---- SearchIcon tests ----

TEST(SearchIconTests, NoneHasNoIcon)
{
    auto icon = SearchIcon::None();
    EXPECT_FALSE(icon.HasTexture());
    EXPECT_FALSE(icon.HasCssClass());
    EXPECT_FALSE(icon.HasAny());
}

TEST(SearchIconTests, FromClassHasCssClassOnly)
{
    auto icon = SearchIcon::FromClass("icon-texture");
    EXPECT_TRUE(icon.HasCssClass());
    EXPECT_FALSE(icon.HasTexture());
    EXPECT_TRUE(icon.HasAny());
    EXPECT_EQ(icon.CssClass, "icon-texture");
}

TEST(SearchIconTests, FromTextureHasTextureOnly)
{
    auto icon = SearchIcon::FromTexture(42);
    EXPECT_FALSE(icon.HasCssClass());
    EXPECT_TRUE(icon.HasTexture());
    EXPECT_TRUE(icon.HasAny());
    EXPECT_EQ(icon.TextureHandle, 42u);
}

TEST(SearchIconTests, BothFieldsCanBeSet)
{
    SearchIcon icon;
    icon.CssClass = "icon-model";
    icon.TextureHandle = 99;
    EXPECT_TRUE(icon.HasCssClass());
    EXPECT_TRUE(icon.HasTexture());
    EXPECT_TRUE(icon.HasAny());
}

// ---- SearchResultItem tests ----

TEST(SearchResultItemTests, UserDataRoundTripsInt)
{
    SearchResultItem item;
    item.UserData = 42;
    EXPECT_EQ(std::any_cast<int>(item.UserData), 42);
}

TEST(SearchResultItemTests, UserDataRoundTripsString)
{
    SearchResultItem item;
    item.UserData = std::string("guid-abc-123");
    EXPECT_EQ(std::any_cast<std::string>(item.UserData), "guid-abc-123");
}

// ---- SearchDialog construction tests ----

TEST(SearchDialogTests, ConstructionCreatesStructure)
{
    SearchDialog dialog;
    EXPECT_TRUE(dialog.HasClass("search-dialog"));
    EXPECT_FALSE(dialog.IsOpen());
}

TEST(SearchDialogTests, ShowOpensAndCloseCloses)
{
    SearchDialog dialog;
    EXPECT_FALSE(dialog.IsOpen());

    dialog.Show();
    EXPECT_TRUE(dialog.IsOpen());
    EXPECT_TRUE(dialog.HasClass("open"));

    dialog.Close();
    EXPECT_FALSE(dialog.IsOpen());
    EXPECT_FALSE(dialog.HasClass("open"));
}

TEST(SearchDialogTests, ShowIsIdempotent)
{
    SearchDialog dialog;
    dialog.Show();
    dialog.Show(); // second call should be a no-op
    EXPECT_TRUE(dialog.IsOpen());
}

TEST(SearchDialogTests, CloseIsIdempotent)
{
    SearchDialog dialog;
    dialog.Close(); // should not crash when not open
    EXPECT_FALSE(dialog.IsOpen());
}

// ---- Provider integration ----

TEST(SearchDialogTests, SetProviderAcceptsNullptr)
{
    SearchDialog dialog;
    dialog.SetProvider(nullptr);
    dialog.Show();
    // Should not crash when querying without a provider
}

TEST(SearchDialogTests, CloseCancelsProvider)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(5));

    SearchDialog dialog;
    dialog.SetProvider(&provider);
    dialog.Show();

    dialog.Close();
    EXPECT_GE(provider.m_CancelCount, 1);
}

TEST(SearchDialogTests, FilterSelectionPrependsProviderQueryWithoutChangingVisibleQuery)
{
    TestSearchProvider provider;
    SearchDialog dialog;
    dialog.SetProvider(&provider);
    dialog.SetFilterOptions({
        {"all", "All fields", ""},
        {"assets", "Assets", "asset:"},
    });
    dialog.Show();

    auto* filter = dynamic_cast<Dropdown*>(FindFirstWithClass(&dialog, "search-dialog-filter"));
    ASSERT_NE(filter, nullptr);
    filter->SetSelectedIndex(1);

    EXPECT_EQ(provider.m_LastQuery, "asset:");
}

TEST(SearchDialogTests, FilterAndFieldAreSiblingsInSharedSearchChrome)
{
    SearchDialog dialog;
    dialog.SetFilterOptions({
        {"all", "All fields", ""},
        {"assets", "Assets", "asset:"},
    });

    UIElement* searchBar = FindFirstWithClass(&dialog, "search-field-with-filter");
    UIElement* field = FindFirstWithClass(&dialog, "search-field-with-filter-input");
    UIElement* filter = FindFirstWithClass(&dialog, "search-field-with-filter-dropdown");
    ASSERT_NE(searchBar, nullptr);
    ASSERT_NE(field, nullptr);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(field->GetParent(), searchBar);
    EXPECT_EQ(filter->GetParent(), searchBar);
}

// ---- Keyboard interaction ----

TEST(SearchDialogTests, EscapeClosesDialogAndFiresCancel)
{
    SearchDialog dialog;
    int cancelCount = 0;
    dialog.SetOnCancel([&cancelCount]() { ++cancelCount; });

    dialog.Show();
    EXPECT_TRUE(dialog.IsOpen());

    SendKey(dialog, Input::kKeyCode_Escape);
    EXPECT_FALSE(dialog.IsOpen());
    EXPECT_EQ(cancelCount, 1);
}

TEST(SearchDialogTests, EscapeDoesNothingWhenClosed)
{
    SearchDialog dialog;
    int cancelCount = 0;
    dialog.SetOnCancel([&cancelCount]() { ++cancelCount; });

    // Not opened — key event should be ignored
    SendKey(dialog, Input::kKeyCode_Escape);
    EXPECT_FALSE(dialog.IsOpen());
    EXPECT_EQ(cancelCount, 0);
}

TEST(SearchDialogTests, EnterWithNoResultsDoesNotCrash)
{
    SearchDialog dialog;
    dialog.Show();

    // No provider, no results — Enter should be harmless
    SendKey(dialog, Input::kKeyCode_Enter);
    EXPECT_TRUE(dialog.IsOpen()); // stays open
}

TEST(SearchDialogTests, InitialSelectionHighlightsMatchingResult)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(20));

    SearchDialog dialog;
    dialog.SetProvider(&provider);
    dialog.SetInitialSelection(7);
    SearchItemId chosen = 0;
    dialog.SetOnResult([&chosen](const SearchResultItem& item) { chosen = item.Id; });

    dialog.Show();
    SendKey(dialog, Input::kKeyCode_Enter);
    EXPECT_EQ(chosen, 7u);
}

TEST(SearchDialogTests, InitialSelectionAbsentKeepsFirstRow)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(20));

    SearchDialog dialog;
    dialog.SetProvider(&provider);
    dialog.SetInitialSelection(999);
    SearchItemId chosen = 0;
    dialog.SetOnResult([&chosen](const SearchResultItem& item) { chosen = item.Id; });

    dialog.Show();
    SendKey(dialog, Input::kKeyCode_Enter);
    EXPECT_EQ(chosen, 1u);
}

TEST(SearchDialogTests, ArrowKeysDoNotCrashWithNoResults)
{
    SearchDialog dialog;
    dialog.Show();

    SendKey(dialog, Input::kKeyCode_Down);
    SendKey(dialog, Input::kKeyCode_Up);
    EXPECT_TRUE(dialog.IsOpen());
}

// ---- Synchronous query flow (headless / no UIManager) ----

TEST(SearchDialogTests, ProviderSearchCountIsTracked)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(5));

    // Verify the provider tracks searches correctly when exercised directly
    std::vector<SearchResultItem> results;
    provider.BeginSearch("Item", [&](std::vector<SearchResultItem> batch, bool /*isComplete*/) {
        results = std::move(batch);
    });

    EXPECT_EQ(provider.m_SearchCount, 1);
    EXPECT_EQ(provider.m_LastQuery, "Item");
    EXPECT_EQ(results.size(), 5u); // All items contain "Item"
}

// ---- ISearchProvider / TestSearchProvider behavior ----

TEST(SearchProviderTests, SinkDeliversAllMatchingResults)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(3));

    std::vector<SearchResultItem> delivered;
    bool completed = false;
    provider.BeginSearch("Item", [&](std::vector<SearchResultItem> batch, bool isComplete) {
        delivered.insert(delivered.end(), batch.begin(), batch.end());
        completed = isComplete;
    });

    EXPECT_TRUE(completed);
    EXPECT_EQ(delivered.size(), 3u);
    EXPECT_EQ(delivered[0].Label, "Item0");
    EXPECT_EQ(std::any_cast<int>(delivered[0].UserData), 0);
}

TEST(SearchProviderTests, SinkDeliversUserData)
{
    TestSearchProvider provider;

    std::vector<SearchResultItem> items;
    SearchResultItem item;
    item.Id = 100;
    item.Label = "TestAsset";
    item.UserData = std::string("my-guid-value");
    items.push_back(std::move(item));
    provider.SetItems(std::move(items));

    std::string receivedGuid;
    bool gotResult = false;

    provider.BeginSearch("Test", [&](std::vector<SearchResultItem> batch, bool /*isComplete*/) {
        if (!batch.empty())
        {
            gotResult = true;
            receivedGuid = std::any_cast<std::string>(batch[0].UserData);
        }
    });

    EXPECT_TRUE(gotResult);
    EXPECT_EQ(receivedGuid, "my-guid-value");
}

TEST(SearchProviderTests, FiltersResultsBySubstring)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(10));

    std::vector<SearchResultItem> results;
    provider.BeginSearch("Item5", [&](std::vector<SearchResultItem> batch, bool /*isComplete*/) {
        results = std::move(batch);
    });

    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].Label, "Item5");
}

TEST(SearchProviderTests, ReturnsAllOnEmptyQuery)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(5));

    std::vector<SearchResultItem> results;
    provider.BeginSearch("", [&](std::vector<SearchResultItem> batch, bool /*isComplete*/) {
        results = std::move(batch);
    });

    EXPECT_EQ(results.size(), 5u);
}

TEST(SearchProviderTests, TracksSearchAndCancelCounts)
{
    TestSearchProvider provider;
    provider.SetItems(MakeTestItems(3));

    EXPECT_EQ(provider.m_SearchCount, 0);
    EXPECT_EQ(provider.m_CancelCount, 0);

    provider.BeginSearch("test", [](auto, auto) {});
    EXPECT_EQ(provider.m_SearchCount, 1);

    provider.CancelSearch();
    EXPECT_EQ(provider.m_CancelCount, 1);
}

TEST(SearchProviderTests, GetPlaceholderTextReturnsDefault)
{
    TestSearchProvider provider;
    EXPECT_EQ(provider.GetPlaceholderText(), "Test search...");
}

TEST(SearchProviderTests, GetPlaceholderTextReturnsCustom)
{
    TestSearchProvider provider;
    provider.SetPlaceholder("Find materials...");
    EXPECT_EQ(provider.GetPlaceholderText(), "Find materials...");
}

// ---- Backdrop dismiss ----

TEST(SearchDialogTests, BackdropClickClosesAndFiresCancel)
{
    SearchDialog dialog;
    int cancelCount = 0;
    dialog.SetOnCancel([&cancelCount]() { ++cancelCount; });

    dialog.Show();
    EXPECT_TRUE(dialog.IsOpen());

    // The backdrop is the first child of the dialog
    auto& children = dialog.GetChildren();
    ASSERT_GE(children.size(), 1u);
    UIElement* backdrop = children[0].get();
    ASSERT_TRUE(backdrop->HasClass("search-dialog-backdrop"));

    // Simulate mouse down on backdrop
    UIEvent e{};
    e.Id = kEventMouseDown;
    e.X = 5.0f;
    e.Y = 5.0f;
    e.Target = backdrop;
    e.CurrentTarget = backdrop;
    backdrop->DispatchEvent(e);

    EXPECT_FALSE(dialog.IsOpen());
    EXPECT_EQ(cancelCount, 1);
}

TEST(SearchDialogTests, PanelClickDoesNotClose)
{
    SearchDialog dialog;
    dialog.Show();

    // The panel is the second child (after backdrop)
    auto& children = dialog.GetChildren();
    ASSERT_GE(children.size(), 2u);
    UIElement* panel = children[1].get();
    ASSERT_TRUE(panel->HasClass("search-dialog-panel"));

    // Simulate mouse down on panel — should be stopped, dialog stays open
    UIEvent e{};
    e.Id = kEventMouseDown;
    e.X = 50.0f;
    e.Y = 50.0f;
    e.Target = panel;
    e.CurrentTarget = panel;
    panel->DispatchEvent(e);

    EXPECT_TRUE(dialog.IsOpen());
}
