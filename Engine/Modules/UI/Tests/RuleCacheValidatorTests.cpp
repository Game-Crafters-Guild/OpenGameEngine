// Cache-correctness validator (GE_UI_RULE_CACHE_VALIDATE) tests.
//
// The validator walks both the cached and live-filtered match paths inside
// CSSParser::ComputeStyleInto and logs a divergence whenever a rule appears
// in one set but not the other. These tests exercise divergence-prone shapes
// (sibling combinator, :empty flip, :nth-child) and assert the divergence
// counter stays at zero across mutations — which is only true if the cache
// invalidation logic correctly catches each structural change.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>

#include "Rendering/Core/Device.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

// The rule-cache validator (CSSParser::*RuleCacheValidation* hooks) is compiled
// only where GE_DEBUG_INSTRUMENTATION is 1 (CSSParser.h guards them with it).
// These tests reference those hooks directly, so the whole suite follows the
// same switch — in Release / RelWithDebInfo the file compiles to no tests.
// Without this guard UIStyleTests cannot build in any optimized config.
#if GE_DEBUG_INSTRUMENTATION

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace
{

std::filesystem::path MakeTempCssPath(const char* prefix)
{
    const auto tmpDir = std::filesystem::temp_directory_path();
    const uint64_t t = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const uint64_t tid = (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
    return tmpDir / (std::string(prefix) + "_" + std::to_string(t) + "_" + std::to_string(tid) + ".css");
}

// Test fixture: enables the validator for the duration of each test and
// resets the divergence counter at SetUp. Tests must call ResetDivergences
// after the steady-state cascade and before each mutation cycle if they
// want to scope assertions to a specific mutation.
class RuleCacheValidatorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        UIRegistration::RegisterBuiltInControls();
        CSSParser::SetRuleCacheValidationEnabledForTest(true);
        CSSParser::ResetRuleCacheValidationDivergenceCount();
        ASSERT_TRUE(CSSParser::IsRuleCacheValidationEnabled());
    }

    void TearDown() override
    {
        CSSParser::ClearRuleCacheValidationOverrideForTest();
    }

    void ResetDivergences()
    {
        CSSParser::ResetRuleCacheValidationDivergenceCount();
    }

    uint64_t Divergences() const
    {
        return CSSParser::GetRuleCacheValidationDivergenceCount();
    }
};

} // namespace

// Sibling combinator: `.toggle + .target` matches when .toggle is the
// preceding sibling. Flipping .toggle's class must invalidate the
// following sibling's cache, otherwise the second cascade would fire a
// validator divergence (cache says rule still matches; live says it no
// longer does, or vice versa).
TEST_F(RuleCacheValidatorTest, AdjacentSiblingComboInvalidatesFollowingSibling)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_validator_adjacent_sib");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#a, #b { width: 100px; height: 40px; }
.toggle + #b { background-color: rgb(255, 0, 0); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    ResetDivergences();

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);
    UIElement* a = r->FindById("a");
    UIElement* b = r->FindById("b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Mutation 1: add .toggle to #a → `.toggle + #b` should now match #b.
    a->AddClass("toggle");
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "AddClass .toggle on #a — sibling combinator invalidation should be sound";

    // Mutation 2: remove .toggle → rule no longer matches #b.
    a->RemoveClass("toggle");
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "RemoveClass .toggle on #a — sibling combinator invalidation should be sound";
}

// General sibling combinator: `.x ~ .y` matches when any preceding
// sibling has .x. Same invalidation contract applies, but the bug
// surface is broader — every following sibling's cache must update.
TEST_F(RuleCacheValidatorTest, GeneralSiblingComboInvalidatesAllFollowingSiblings)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' />
        <uielement id='b' />
        <uielement id='c' />
        <uielement id='d' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_validator_general_sib");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 400px; height: 40px; }
#a, #b, #c, #d { width: 100px; height: 40px; }
.marker ~ uielement { background-color: rgb(0, 255, 0); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    ResetDivergences();

    UIElement* r = ui.GetRootElement();
    UIElement* a = r->FindById("a");
    ASSERT_NE(a, nullptr);

    a->AddClass("marker");
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "AddClass .marker on #a — all following siblings (#b, #c, #d) must invalidate";

    a->RemoveClass("marker");
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "RemoveClass .marker on #a — all following siblings must invalidate";
}

// :empty pseudo: matches when an element has no children. Adding /
// removing children flips the match. The structural matcher is recursive
// (the parent's :empty depends on the child set), so the parent's cache
// must invalidate when its child set changes — not just the descendants'.
TEST_F(RuleCacheValidatorTest, EmptyPseudoFlipOnChildInsertRemove)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    const std::string xml = R"(<uielement id='root'>
        <uielement id='box' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_validator_empty");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
#box { width: 100px; height: 40px; }
#box:empty { background-color: rgb(0, 0, 255); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    ResetDivergences();

    UIElement* r = ui.GetRootElement();
    UIElement* box = r->FindById("box");
    ASSERT_NE(box, nullptr);

    // Mutation 1: insert a child → :empty no longer matches #box.
    auto child = std::make_unique<UIElement>();
    child->SetId("child");
    UIElement* childRaw = child.get();
    box->AddChild(std::move(child));
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "AddChild flips :empty match on parent — parent cache must invalidate";

    // Mutation 2: remove the child → :empty matches again.
    box->RemoveChild(childRaw);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "RemoveChild restores :empty match on parent — parent cache must invalidate";
}

// :nth-child(N) is a structural pseudo whose match depends on the
// element's index in its parent's child list. Inserting a sibling
// before the targeted element shifts every following sibling's index,
// so every following sibling's cache must invalidate.
TEST_F(RuleCacheValidatorTest, NthChildInvalidatesOnSiblingInsert)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    const std::string xml = R"(<uielement id='root'>
        <uielement id='c1' />
        <uielement id='c2' />
        <uielement id='c3' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_validator_nth_child");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 400px; height: 40px; }
#root > uielement { width: 100px; height: 40px; }
#root > uielement:nth-child(2) { background-color: rgb(255, 255, 0); }
#root > uielement:nth-child(odd) { color: rgb(128, 0, 128); }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    ResetDivergences();

    UIElement* r = ui.GetRootElement();
    ASSERT_NE(r, nullptr);

    // Mutation: insert a new sibling at the front. Every following
    // sibling shifts index by 1.
    auto newFirst = std::make_unique<UIElement>();
    newFirst->SetId("c0");
    r->InsertChild(0, std::move(newFirst));
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "Inserting a sibling at index 0 shifts :nth-child for every following sibling";

    // Mutation: remove the inserted sibling. Indices shift back.
    UIElement* inserted = r->FindById("c0");
    ASSERT_NE(inserted, nullptr);
    r->RemoveChild(inserted);
    ui.Update(0.0f, /*interactive=*/true);
    EXPECT_EQ(Divergences(), 0u) << "Removing the inserted sibling shifts :nth-child back for the rest";
}

// Idle frames (no mutations) must not produce divergences. This is the
// baseline check — if it fires, the validator itself is buggy or
// BuildRuleCache disagrees with the live walk on first cascade.
TEST_F(RuleCacheValidatorTest, IdleFramesProduceNoDivergences)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "Device init failed";

    const std::string xml = R"(<uielement id='root'>
        <uielement id='a' class='foo' />
        <uielement id='b' />
    </uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));

    UIManager ui(dev);
    ui.SetRoot(std::move(root));

    const auto cssPath = MakeTempCssPath("ui_validator_idle");
    {
        std::ofstream f(cssPath);
        f << R"(
#root { display: flex; width: 300px; height: 40px; }
.foo { background-color: rgb(255, 128, 64); }
.foo + uielement { color: rgb(64, 128, 255); }
uielement:nth-child(1) { font-weight: bold; }
)";
    }
    ASSERT_TRUE(ui.AttachStyleFromFile(cssPath.string()));

    ui.Update(0.0f, /*interactive=*/true);
    ResetDivergences();

    for (int i = 0; i < 4; ++i)
        ui.Update(0.0f, /*interactive=*/true);

    EXPECT_EQ(Divergences(), 0u) << "Idle frames must not produce cache-vs-live divergences";
}

#endif // GE_DEBUG_INSTRUMENTATION

// A cascade run against an EMPTY stylesheet pool must not publish a
// per-element rule cache. An empty pool proves nothing about which rules
// match, so recording the empty result as authoritative makes every later
// cascade match nothing too, including the ones that carry the real sheets.
//
// This test lives outside the GE_DEBUG_INSTRUMENTATION guard above: it needs no validator hook,
// and the invariant it pins holds in every config.
TEST(RuleCacheEmptyPool, EmptySheetPoolDoesNotPoisonTheRuleCache)
{
    auto* dev = SharedHeadlessDevice();
    if (!dev) GTEST_SKIP() << "Device init failed";
    GameEngine::UIRegistration::RegisterBuiltInControls();

    GameEngine::Stylesheet sheet{};
    ASSERT_TRUE(GameEngine::UIParsing::CSSParser::ParseStylesFromString(
        ".property-slider { height: 24px; }\n", sheet));

    GameEngine::UIManager ui(dev);
    auto root = std::make_unique<GameEngine::UIElement>();
    auto owned = std::make_unique<GameEngine::UIElement>();
    owned->AddClass("property-slider");
    GameEngine::UIElement* el = owned.get();
    root->AddChild(std::move(owned));
    ui.SetRoot(std::move(root));
    // The element needs an owning manager: IsRuleCacheValid() is false without
    // one, so a manager-less element never consults the cache at all.
    ui.Update(0.0f, /*interactive=*/true);
    ASSERT_EQ(el->GetOwnerManager(), &ui);
    // That update ran the manager's own cascade over the sheets it holds (the UI module's
    // default stylesheet) and published a cache for them. Drop it, so the two calls below
    // start from an unpublished cache, which is the state the defect needs.
    el->InvalidateRuleCacheSubtree();

    const GameEngine::Stylesheet* sheets[] = {&sheet};
    const GameEngine::UIParsing::ElementState state{};

    // A caller whose interned sheet-set id no longer resolves. Matching
    // nothing is the right answer for THIS call; the defect is publishing it.
    GameEngine::ResolvedStyle empty{};
    GameEngine::UIParsing::CSSParser::ComputeStyleInto(empty, *el, {}, {}, state,
                                                       /*parentStyle=*/nullptr);
    EXPECT_FALSE(empty.Layout.Height.IsPx())
        << "Precondition: an empty pool cannot resolve the class rule.";

    // The next caller DOES have the sheet. It must match.
    GameEngine::ResolvedStyle real{};
    GameEngine::UIParsing::CSSParser::ComputeStyleInto(real, *el, sheets, {}, state,
                                                       /*parentStyle=*/nullptr);
    ASSERT_TRUE(real.Layout.Height.IsPx())
        << "The cascade trusted a rule cache built from an empty sheet pool.";
    EXPECT_NEAR(real.Layout.Height.Value, 24.f, 0.01f);
}
