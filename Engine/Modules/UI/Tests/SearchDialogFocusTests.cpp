#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/WindowInputRouter.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/SearchFieldWithFilter.h"
#include "UI/Controls/TextField.h"
#include "UI/Interaction/FocusIsInside.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <filesystem>

using namespace GameEngine;

namespace
{
class OneResultProvider : public ISearchProvider
{
public:
    void BeginSearch(const std::string&, ResultSink sink) override
    {
        SearchResultItem item;
        item.Id = 1;
        item.Label = "Gate";
        sink({item}, true);
    }
    void CancelSearch() override {}
    std::string GetPlaceholderText() const override { return "Search"; }
};

UIElement* FindClass(UIElement& element, const char* name)
{
    if (element.HasClass(name)) return &element;
    for (const auto& child : element.GetChildren())
        if (auto* found = FindClass(*child, name)) return found;
    return nullptr;
}

class SearchDialogFocusTests : public ::testing::Test
{
protected:
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    Rendering::IDevice* device = nullptr;
    std::unique_ptr<UIManager> ui;
    SearchDialog* dialog = nullptr;
    TextField* other = nullptr;
    SearchFieldWithFilter* search = nullptr;
    OneResultProvider provider;

    void SetUp() override
    {
        device = SharedHeadlessDevice();
        if (!device)
            GTEST_SKIP() << "No headless Vulkan device";
        ui = std::make_unique<UIManager>(device);
        auto root = std::make_unique<UIElement>();
        auto field = std::make_unique<TextField>();
        other = field.get();
        other->SetId("other-field");
        root->AddChild(std::move(field));
        auto popup = std::make_unique<SearchDialog>();
        dialog = popup.get();
        search = dynamic_cast<SearchFieldWithFilter*>(FindClass(*dialog, "search-field-with-filter"));
        ASSERT_NE(search, nullptr);
        root->AddChild(std::move(popup));
        ui->SetRoot(std::move(root));
    }

    void Drain() { ui->DrainDeferredActionsOnce(); }
};

class SearchDialogPointerFocusTests : public SearchDialogFocusTests,
                                      public ::testing::WithParamInterface<bool> {};
} // namespace

TEST_F(SearchDialogFocusTests, EscapeReleasesFocusBeforeNextGameplayKey)
{
    Input::InputSystem game;
    constexpr auto context = Input::HashInput("Test.Game");
    constexpr auto ability = Input::HashInput("Test.Ability");
    game.BindKey(context, ability, {Input::DeviceType::Keyboard, Input::kKeyCode_Q, 1.0f});
    game.PushContext(context);
    WindowInputRouterConfig config;
    config.getUi = [this] { return ui.get(); };
    config.getPlaySurface = [&game]
    {
        WindowInputRouterConfig::PlaySurface surface;
        surface.gameplaySink = &game;
        return surface;
    };

    dialog->Show();
    Drain();
    ASSERT_TRUE(UI::FocusIsInside(*dialog));
    WindowInputRouter::RouteKey(config, Input::kKeyCode_Escape, 1, 0);
    EXPECT_FALSE(dialog->IsOpen());
    EXPECT_TRUE(ui->GetFocusedElementId().empty());
    WindowInputRouter::RouteKey(config, Input::kKeyCode_Escape, 0, 0);
    WindowInputRouter::RouteKey(config, Input::kKeyCode_Q, 1, 0);
    WindowInputRouter::RouteKey(config, Input::kKeyCode_Q, 0, 0);
    game.Update(0.016f);
    EXPECT_TRUE(game.WasActionTriggered(ability));
    EXPECT_FALSE(ui->OnChar('q'));
    EXPECT_TRUE(search->GetValue().empty());
}

TEST_F(SearchDialogFocusTests, CloseBeforeDeferredDrainCannotRefocusHiddenField)
{
    dialog->Show();
    dialog->Close();
    Drain();
    EXPECT_TRUE(ui->GetFocusedElementId().empty());
}

TEST_F(SearchDialogFocusTests, ClosePreservesFocusAlreadyTransferredOutside)
{
    dialog->Show();
    ui->FocusElement(other);
    dialog->Close();
    Drain();
    EXPECT_EQ(ui->GetFocusedElementId(), other->GetId());
}

TEST_F(SearchDialogFocusTests, ReopenDoesNotReplayOldFocusRequests)
{
    dialog->Show();
    dialog->Close();
    dialog->Show();
    ASSERT_TRUE(UI::FocusIsInside(*dialog));
    ui->FocusElement(other);
    Drain();
    EXPECT_EQ(ui->GetFocusedElementId(), other->GetId());
}

TEST_F(SearchDialogFocusTests, CancelCallbackOwnsItsFocusTransfer)
{
    dialog->SetOnCancel([this] { ui->FocusElement(other); });
    dialog->Show();
    ui->OnKey(Input::kKeyCode_Escape, 1, 0);
    Drain();
    EXPECT_FALSE(dialog->IsOpen());
    EXPECT_EQ(ui->GetFocusedElementId(), other->GetId());
}

// Each control that focuses the query field does so while the dialog is open;
// none may queue a refocus that lands after Close.
enum class QueryFocusTrigger
{
    ViewToggle,
    FilterChange,
    Clear,
};

class SearchDialogRefocusAfterCloseTests : public SearchDialogFocusTests,
                                           public ::testing::WithParamInterface<QueryFocusTrigger> {};

TEST_P(SearchDialogRefocusAfterCloseTests, ATriggerBeforeCloseCannotRefocusTheHiddenField)
{
    dialog->SetFilterOptions({{"all", "All", ""}, {"assets", "Assets", "asset:"}});
    dialog->Show();
    Drain();
    switch (GetParam())
    {
    case QueryFocusTrigger::ViewToggle:
    {
        auto* toggle = dynamic_cast<Button*>(FindClass(*dialog, "search-dialog-result-view-toggle"));
        ASSERT_NE(toggle, nullptr);
        toggle->TriggerClick();
        break;
    }
    case QueryFocusTrigger::FilterChange:
        search->GetFilter()->SetSelectedIndex(1);
        break;
    case QueryFocusTrigger::Clear:
        search->SetValue("gate");
        search->GetClearButton()->TriggerClick();
        EXPECT_TRUE(search->GetValue().empty());
        break;
    }
    dialog->Close();
    Drain();
    EXPECT_TRUE(ui->GetFocusedElementId().empty());
}

INSTANTIATE_TEST_SUITE_P(Triggers, SearchDialogRefocusAfterCloseTests,
    ::testing::Values(QueryFocusTrigger::ViewToggle, QueryFocusTrigger::FilterChange, QueryFocusTrigger::Clear),
    [](const ::testing::TestParamInfo<QueryFocusTrigger>& info)
    {
        switch (info.param)
        {
        case QueryFocusTrigger::ViewToggle: return "ViewToggle";
        case QueryFocusTrigger::FilterChange: return "FilterChange";
        case QueryFocusTrigger::Clear: return "Clear";
        }
        return "Unknown";
    });

TEST_F(SearchDialogFocusTests, ClearCallbackCanCloseAndTransferFocus)
{
    dialog->Show();
    Drain();
    search->SetValue("gate");
    search->SetOnQueryChanging([this](const std::string&) {
        dialog->Close();
        ui->FocusElement(other);
    });
    search->GetClearButton()->TriggerClick();
    Drain();
    EXPECT_EQ(ui->GetFocusedElementId(), other->GetId());
}

TEST_F(SearchDialogFocusTests, ClearStillFocusesAnOpenQueryField)
{
    dialog->Show();
    Drain();
    search->SetValue("gate");
    ui->FocusElement(other);
    search->GetClearButton()->TriggerClick();
    Drain();
    EXPECT_TRUE(dialog->IsOpen());
    EXPECT_EQ(ui->GetFocusedElementId(), search->GetField()->GetId());
}

TEST_F(SearchDialogFocusTests, ClearHonorsDisabledRefocusPolicy)
{
    dialog->Show();
    Drain();
    search->SetValue("gate");
    search->SetRefocusAfterClear(false);
    ui->FocusElement(other);
    search->GetClearButton()->TriggerClick();
    Drain();
    EXPECT_TRUE(search->GetValue().empty());
    EXPECT_EQ(ui->GetFocusedElementId(), other->GetId());
}

TEST_F(SearchDialogFocusTests, DestroyBeforeDeferredDrainDoesNotTouchReplacement)
{
    dialog->Show();
    auto replacement = std::make_unique<TextField>();
    replacement->SetId(search->GetField()->GetId());
    ui->SetRoot(std::move(replacement));
    ui->SetFocusById({});
    Drain();
    EXPECT_TRUE(ui->GetFocusedElementId().empty());
}

TEST_F(SearchDialogFocusTests, ResultCallbackCanTransferFocus)
{
    dialog->SetProvider(&provider);
    bool selected = false;
    dialog->SetOnResult([this, &selected](const SearchResultItem& item) {
        EXPECT_EQ(item.Id, 1u);
        EXPECT_FALSE(dialog->IsOpen());
        EXPECT_TRUE(ui->GetFocusedElementId().empty());
        selected = true;
        ui->FocusElement(other);
    });
    dialog->Show();
    Drain();
    ui->OnKey(Input::kKeyCode_Enter, Input::kKeyActionPress, 0);
    Drain();
    EXPECT_TRUE(selected);
    EXPECT_EQ(ui->GetFocusedElementId(), other->GetId());
}

TEST_F(SearchDialogFocusTests, KeepOpenResultRetainsQueryFocus)
{
    dialog->SetProvider(&provider);
    dialog->SetShouldCloseOnResult([] { return false; });
    bool selected = false;
    dialog->SetOnResult([&selected](const SearchResultItem&) { selected = true; });
    dialog->Show();
    Drain();
    ui->OnKey(Input::kKeyCode_Enter, Input::kKeyActionPress, 0);
    Drain();
    EXPECT_TRUE(selected);
    EXPECT_TRUE(dialog->IsOpen());
    EXPECT_TRUE(UI::FocusIsInside(*dialog));
}

TEST_P(SearchDialogPointerFocusTests, ResultPointerReleaseDoesNotRefocusHiddenResults)
{
    ui->GetRootElement()->Overrides()
        .Set(Style::Width, StyleLength::Px(800.0f))
        .Set(Style::Height, StyleLength::Px(600.0f));
    const auto css = PathUtils::GetExecutableDirectory() / "Assets" / "UI" / "controls" / "SearchDialog.css";
    ASSERT_TRUE(ui->AttachStyleFromFile(css.string()));
    // The universal search palette reserves the full result viewport. A
    // compact one-row dialog can clip a grid card taller than a list row.
    dialog->SetFixedHeight(true);
    dialog->SetProvider(&provider);
    int selected = 0;
    dialog->SetOnResult([&selected](const SearchResultItem&) { ++selected; });
    dialog->Show();
    if (GetParam())
    {
        auto* toggle = dynamic_cast<Button*>(FindClass(*dialog, "search-dialog-result-view-toggle"));
        ASSERT_NE(toggle, nullptr);
        toggle->TriggerClick();
    }
    UiRgHarness rg(device);
    for (int i = 0; i < 5; ++i)
    {
        ui->Update(0.0f, false);
        DriveUiRender(*ui, rg);
    }
    auto* list = dynamic_cast<ListView*>(FindClass(*dialog, "search-dialog-results"));
    ASSERT_NE(list, nullptr);
    UIElement* cell = GetParam() ? FindClass(*dialog, "search-result-grid-card") : list->GetCellForIndex(0);
    ASSERT_NE(cell, nullptr);
    ASSERT_GT(cell->GetLayoutWidth(), 8.0f);
    ASSERT_GT(cell->GetLayoutHeight(), 0.0f);
    ui->OnMouseMove(cell->GetLayoutX() + 4.0f,
                    cell->GetLayoutY() + cell->GetLayoutHeight() * 0.5f);
    ui->OnMouseButton(0, true);
    if (GetParam())
    {
        EXPECT_EQ(selected, 0) << "Grid mode must exercise activation during release";
        ASSERT_TRUE(ui->IsMouseCaptured()) << ui->GetHoveredElementDebugName();
    }
    ui->OnMouseButton(0, false);
    EXPECT_EQ(selected, 1);
    EXPECT_FALSE(dialog->IsOpen());
    EXPECT_TRUE(ui->GetFocusedElementId().empty());
    Drain();
    EXPECT_TRUE(ui->GetFocusedElementId().empty());
}

INSTANTIATE_TEST_SUITE_P(ResultModes, SearchDialogPointerFocusTests, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "Grid" : "List"; });
