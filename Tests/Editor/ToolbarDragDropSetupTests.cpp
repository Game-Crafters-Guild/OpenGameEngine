// Toolbar drag-drop container matching, across the repeated Setup the toolbar performs.
//
// SceneViewToolbar::OnPostLayout calls ToolbarDragDrop::Setup on every layout pass, by
// design: UXML mounts buttons late, and a button that appears after the first pass still
// needs drag handlers. That makes Setup a per-frame path, so it resolves its containers by
// precomputed class id and reads the caller's config through the object rather than copying
// it into each handler's captures.
//
// What these arms hold down is the behaviour that refactor must not change: a button is
// draggable exactly when its parent is one of the registered containers, and that stays true
// after Setup has re-run. The membership test is the part that moved — from a per-handler
// copy of the config to m_Containers' ClassId — so it is the part worth pinning.
//
// "The drag armed" is observed through the ghost element: the mouse-move handler creates the
// drag ghost and insertion indicator under the ghost root once the pointer passes the drag
// threshold, and creates nothing at all for a button it never armed on.

#include <gtest/gtest.h>

#include "UI/Controls/Button.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/StyleProperties.h"
#include "UI/ToolbarDragDrop.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using GameEngine::Button;
using GameEngine::DisplayMode;
using GameEngine::UIElement;
using GameEngine::UIEvent;
using GameEngine::UILayoutAccess;
using GameEngine::Editor::ToolbarDragDrop;

namespace
{
constexpr const char* kRegisteredContainerClass = "sceneview-toolbar-right";
constexpr const char* kRegisteredSettingsKey = "ui.sceneview.toolbar.order";
constexpr const char* kStrangerContainerClass = "some-other-toolbar";

const std::vector<ToolbarDragDrop::ContainerConfig>& Containers()
{
    static const std::vector<ToolbarDragDrop::ContainerConfig> kConfig = {
        {kRegisteredContainerClass, kRegisteredSettingsKey},
    };
    return kConfig;
}

// Root
//  |- container(kRegisteredContainerClass) -- RegisteredButton
//  |- container(kStrangerContainerClass)   -- StrangerButton
//
// The root doubles as the ghost root, which is what the production wiring does (the ghost
// belongs above everything, so it goes on the global UI root).
struct ToolbarTree
{
    std::unique_ptr<UIElement> Root = std::make_unique<UIElement>();
    Button* RegisteredButton = nullptr;
    Button* StrangerButton = nullptr;

    ToolbarTree()
    {
        RegisteredButton = AddContainerWithButton(kRegisteredContainerClass, "RegisteredButton");
        StrangerButton = AddContainerWithButton(kStrangerContainerClass, "StrangerButton");
    }

    size_t GhostRootChildCount() const { return Root->GetChildren().size(); }

  private:
    Button* AddContainerWithButton(const char* containerClass, const char* buttonId)
    {
        auto container = std::make_unique<UIElement>();
        container->AddClass(containerClass);

        auto button = std::make_unique<Button>();
        button->SetId(buttonId);
        Button* raw = button.get();
        container->AddChild(std::move(button));

        Root->AddChild(std::move(container));
        return raw;
    }
};

// Press, then move far enough to cross the 5px drag threshold. Returns nothing: the
// observable is what appeared under the ghost root.
void PressAndDrag(Button* button)
{
    UIEvent down{};
    down.Id = GameEngine::kEventMouseDown;
    down.Button = 0;
    down.X = 0.0f;
    down.Y = 0.0f;
    down.Target = button;
    down.CurrentTarget = button;
    button->DispatchEvent(down);

    UIEvent move{};
    move.Id = GameEngine::kEventMouseMove;
    move.Button = 0;
    move.X = 80.0f;
    move.Y = 40.0f;
    move.Target = button;
    move.CurrentTarget = button;
    button->DispatchEvent(move);
}
} // namespace

// The baseline the other arms are read against: a button inside a registered container arms
// the drag, and the ghost proves it.
TEST(ToolbarDragDropSetupTests, ButtonInRegisteredContainerArmsTheDrag)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;
    dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());

    const size_t before = tree.GhostRootChildCount();
    PressAndDrag(tree.RegisteredButton);

    EXPECT_GT(tree.GhostRootChildCount(), before)
        << "a button in a registered container never armed the drag — container matching is broken";
}

// The negative control for the same matching rule. Without this, an arm that matched
// *every* container would pass the test above for the wrong reason.
TEST(ToolbarDragDropSetupTests, ButtonOutsideRegisteredContainersDoesNotArm)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;
    dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());

    const size_t before = tree.GhostRootChildCount();
    PressAndDrag(tree.StrangerButton);

    EXPECT_EQ(tree.GhostRootChildCount(), before)
        << "a button outside every registered container armed the drag";
}

// The per-frame path. Setup re-runs on every layout pass, so the config it matches against
// has to survive re-resolution — this is the arm that fails if the container ids or settings
// keys are lost when the rows are reused instead of rebuilt.
TEST(ToolbarDragDropSetupTests, MatchingSurvivesRepeatedSetup)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;
    for (int pass = 0; pass < 5; ++pass)
        dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());

    const size_t before = tree.GhostRootChildCount();
    PressAndDrag(tree.RegisteredButton);

    EXPECT_GT(tree.GhostRootChildCount(), before)
        << "container matching stopped working after Setup re-ran";
}

// A button that mounts after the first pass is the reason Setup re-runs at all. It must be
// picked up, and it must be matched against the same container rules.
TEST(ToolbarDragDropSetupTests, LateMountedButtonIsArmedByALaterSetup)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;
    dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());

    // Mount a second button into the registered container, as async UXML would.
    UIElement* registeredContainer = tree.RegisteredButton->GetParent();
    ASSERT_NE(registeredContainer, nullptr);
    auto lateButton = std::make_unique<Button>();
    lateButton->SetId("LateButton");
    Button* late = lateButton.get();
    registeredContainer->AddChild(std::move(lateButton));

    dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());

    const size_t before = tree.GhostRootChildCount();
    PressAndDrag(late);

    EXPECT_GT(tree.GhostRootChildCount(), before)
        << "a button that mounted after the first Setup never got drag handlers";
}

// The top toolbar hugs its icons so the gaps between them are not menu space, then names
// the stretched section as the drop zone so a drag can still land in the empty bar.
// Hit-testing the hugging container would silently drop that second property: the pointer
// is over empty bar, the drop misses, and the insertion indicator stays hidden.
namespace
{
constexpr const char* kIconRunClass = "toolbar-icons-left";
constexpr const char* kSectionClass = "top-toolbar-left";
constexpr const char* kTopToolbarSettingsKey = "ui.toolbar.left.v3.order";

// Section (400x32) owns the empty bar; the icon run (80x32) hugs the button.
// A pointer at x=200 is inside the section and outside the icon run.
struct HuggingIconTree
{
    std::unique_ptr<UIElement> Root = std::make_unique<UIElement>();
    Button* IconButton = nullptr;
    UIElement* IconRun = nullptr;
    UIElement* Section = nullptr;

    HuggingIconTree()
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass(kSectionClass);
        Section = section.get();

        auto iconRun = std::make_unique<UIElement>();
        iconRun->AddClass(kIconRunClass);
        IconRun = iconRun.get();

        auto button = std::make_unique<Button>();
        button->SetId("TopToolbarLeft1");
        IconButton = button.get();
        iconRun->AddChild(std::move(button));
        section->AddChild(std::move(iconRun));
        Root->AddChild(std::move(section));

        UILayoutAccess::SetLastLayoutRect(*Section, 0.0f, 0.0f, 400.0f, 32.0f);
        UILayoutAccess::SetLastLayoutRect(*IconRun, 0.0f, 0.0f, 80.0f, 32.0f);
        UILayoutAccess::SetLastLayoutRect(*IconButton, 0.0f, 0.0f, 32.0f, 32.0f);
    }
};

void PressAndDragTo(Button* button, float x, float y)
{
    UIEvent down{};
    down.Id = GameEngine::kEventMouseDown;
    down.Button = 0;
    down.X = 16.0f;
    down.Y = 16.0f;
    down.Target = button;
    down.CurrentTarget = button;
    button->DispatchEvent(down);

    UIEvent move{};
    move.Id = GameEngine::kEventMouseMove;
    move.Button = 0;
    move.X = x;
    move.Y = y;
    move.Target = button;
    move.CurrentTarget = button;
    button->DispatchEvent(move);
}

DisplayMode IndicatorDisplay(UIElement* root)
{
    UIElement* indicator = root->FindById("toolbar-insertion-indicator");
    if (!indicator)
        return DisplayMode::None;
    return indicator->Overrides().Get(GameEngine::Style::Display).value_or(DisplayMode::None);
}
} // namespace

TEST(ToolbarDragDropSetupTests, DropZoneCssClassAcceptsADropOutsideTheHuggingContainer)
{
    HuggingIconTree tree;
    ToolbarDragDrop dragDrop;
    const std::vector<ToolbarDragDrop::ContainerConfig> config = {
        {kIconRunClass, kTopToolbarSettingsKey, kSectionClass},
    };
    dragDrop.Setup(tree.Root.get(), config, tree.Root.get());

    PressAndDragTo(tree.IconButton, 200.0f, 16.0f);

    EXPECT_EQ(IndicatorDisplay(tree.Root.get()), DisplayMode::Block)
        << "a pointer in the section's empty bar never hit the drop zone — drops would "
           "only land on the hugging icon run";
}

TEST(ToolbarDragDropSetupTests, HuggingContainerIsTheHitBoxWhenNoDropZoneIsNamed)
{
    HuggingIconTree tree;
    ToolbarDragDrop dragDrop;
    const std::vector<ToolbarDragDrop::ContainerConfig> config = {
        {kIconRunClass, kTopToolbarSettingsKey},
    };
    dragDrop.Setup(tree.Root.get(), config, tree.Root.get());

    PressAndDragTo(tree.IconButton, 200.0f, 16.0f);

    EXPECT_EQ(IndicatorDisplay(tree.Root.get()), DisplayMode::None)
        << "a pointer outside the hugging container still hit a drop — the unnamed "
           "drop-zone fallback is not the container itself";
}

// A .uxml hot reload can destroy a registered container and mount a replacement with the
// same class between two Setup passes. Until the next Setup, a drag must not read the
// destroyed container; after it, the replacement is the drop target.
namespace
{
constexpr const char* kSourceClass = "anim-toolbar-edit";
constexpr const char* kTargetClass = "anim-toolbar-view";

std::unique_ptr<UIElement> MakeTargetContainer()
{
    auto target = std::make_unique<UIElement>();
    target->AddClass(kTargetClass);
    UILayoutAccess::SetLastLayoutRect(*target, 200.0f, 0.0f, 100.0f, 32.0f);
    return target;
}

void Release(Button* button)
{
    UIEvent up{};
    up.Id = GameEngine::kEventMouseUp;
    up.Button = 0;
    up.Target = button;
    up.CurrentTarget = button;
    button->DispatchEvent(up);
}
} // namespace

TEST(ToolbarDragDropSetupTests, ReplacedContainerIsNotReadAndTheNextSetupUsesItsReplacement)
{
    auto root = std::make_unique<UIElement>();
    auto source = std::make_unique<UIElement>();
    source->AddClass(kSourceClass);
    auto button = std::make_unique<Button>();
    button->SetId("SourceButton");
    Button* dragged = button.get();
    source->AddChild(std::move(button));
    UILayoutAccess::SetLastLayoutRect(*source, 0.0f, 0.0f, 80.0f, 32.0f);
    UILayoutAccess::SetLastLayoutRect(*dragged, 0.0f, 0.0f, 32.0f, 32.0f);
    root->AddChild(std::move(source));
    auto first = MakeTargetContainer();
    UIElement* destroyed = first.get();
    root->AddChild(std::move(first));

    ToolbarDragDrop dragDrop;
    const std::vector<ToolbarDragDrop::ContainerConfig> config = {
        {kSourceClass, "ui.test.toolbar.source.order"},
        {kTargetClass, "ui.test.toolbar.target.order"},
    };
    dragDrop.Setup(root.get(), config, root.get());

    root->RemoveChild(destroyed);
    root->AddChild(MakeTargetContainer());

    PressAndDragTo(dragged, 250.0f, 16.0f);
    EXPECT_EQ(IndicatorDisplay(root.get()), DisplayMode::None)
        << "the drag hit-tested the destroyed container instead of skipping it";
    Release(dragged);

    dragDrop.Setup(root.get(), config, root.get());
    PressAndDragTo(dragged, 250.0f, 16.0f);
    EXPECT_EQ(IndicatorDisplay(root.get()), DisplayMode::Block)
        << "the next Setup did not resolve the replacement container";
}

// The drop is posted on mouse-up and runs later. A hot reload that destroys the target
// container or the dragged button in between, or a teardown of the toolbar that owns the
// drag-drop, must cancel the drop rather than act through the destroyed object; the button
// stays where it was.
namespace
{
// Root -- source(kSourceClass) -- SourceButton
//      -- target(kTargetClass), empty, at x 200-300
//
// Drag() takes SourceButton over the target and releases it, which posts the drop to the
// dispatcher the fixture installs; nothing moves until Drain().
struct PostedDropTree
{
    GameEngine::UI::UiDispatcher Dispatcher;
    GameEngine::UI::UiContextScope Scope{&Dispatcher, nullptr};
    std::unique_ptr<UIElement> Root = std::make_unique<UIElement>();
    UIElement* Source = nullptr;
    UIElement* Target = nullptr;
    Button* Dragged = nullptr;
    std::unique_ptr<ToolbarDragDrop> DragDrop = std::make_unique<ToolbarDragDrop>();

    PostedDropTree()
    {
        auto source = std::make_unique<UIElement>();
        source->AddClass(kSourceClass);
        Source = source.get();
        auto button = std::make_unique<Button>();
        button->SetId("SourceButton");
        Dragged = button.get();
        source->AddChild(std::move(button));
        UILayoutAccess::SetLastLayoutRect(*Source, 0.0f, 0.0f, 80.0f, 32.0f);
        UILayoutAccess::SetLastLayoutRect(*Dragged, 0.0f, 0.0f, 32.0f, 32.0f);
        Root->AddChild(std::move(source));
        auto target = MakeTargetContainer();
        Target = target.get();
        Root->AddChild(std::move(target));

        static const std::vector<ToolbarDragDrop::ContainerConfig> kConfig = {
            {kSourceClass, "ui.test.toolbar.source.order"},
            {kTargetClass, "ui.test.toolbar.target.order"},
        };
        DragDrop->Setup(Root.get(), kConfig, Root.get());
    }

    // False when the drag never reached the target, so no drop was posted.
    bool Drag()
    {
        PressAndDragTo(Dragged, 250.0f, 16.0f);
        const bool overTarget = IndicatorDisplay(Root.get()) == DisplayMode::Block;
        Release(Dragged);
        return overTarget;
    }
};
} // namespace

TEST(ToolbarDragDropSetupTests, PostedDropIsCancelledWhenItsTargetContainerIsDestroyed)
{
    PostedDropTree tree;
    ASSERT_TRUE(tree.Drag()) << "the drag never reached the target container, so no drop is posted";

    tree.Root->RemoveChild(tree.Target);
    tree.Dispatcher.Drain();

    ASSERT_EQ(tree.Source->GetChildren().size(), 1u)
        << "the posted drop moved the button into the destroyed container";
    EXPECT_EQ(tree.Source->GetChildren()[0].get(), tree.Dragged);
}

// The reload replaces the dragged button with a new one in the same container. The drop
// was for the destroyed button, so the replacement must stay put.
TEST(ToolbarDragDropSetupTests, PostedDropIsCancelledWhenItsButtonIsDestroyed)
{
    PostedDropTree tree;
    ASSERT_TRUE(tree.Drag()) << "the drag never reached the target container, so no drop is posted";

    tree.Source->RemoveChild(tree.Dragged);
    auto replacement = std::make_unique<Button>();
    replacement->SetId("SourceButton");
    UIElement* const replacementButton = replacement.get();
    tree.Source->AddChild(std::move(replacement));
    tree.Dispatcher.Drain();

    EXPECT_TRUE(tree.Target->GetChildren().empty())
        << "the posted drop moved a button other than the one that was dragged";
    ASSERT_EQ(tree.Source->GetChildren().size(), 1u);
    EXPECT_EQ(tree.Source->GetChildren()[0].get(), replacementButton);
}

TEST(ToolbarDragDropSetupTests, PostedDropIsCancelledWhenTheDragDropIsDestroyed)
{
    PostedDropTree tree;
    ASSERT_TRUE(tree.Drag()) << "the drag never reached the target container, so no drop is posted";

    tree.DragDrop.reset();
    tree.Dispatcher.Drain();

    EXPECT_TRUE(tree.Target->GetChildren().empty())
        << "the posted drop ran after the drag-drop that posted it was destroyed";
}

// Reset returns a container to the authored order LoadButtonOrder snapshotted. A .uxml hot
// reload that replaces the container mounts a new authored order, and Reset must return to
// that one, not to the order of the container it replaced.
namespace
{
constexpr const char* kResetClass = "anim-toolbar-snap";

std::unique_ptr<UIElement> MakeResetContainer(const char* firstId, const char* secondId)
{
    auto container = std::make_unique<UIElement>();
    container->AddClass(kResetClass);
    for (const char* id : {firstId, secondId})
    {
        auto button = std::make_unique<Button>();
        button->SetId(id);
        container->AddChild(std::move(button));
    }
    return container;
}

std::vector<std::string> ChildIds(const UIElement* container)
{
    std::vector<std::string> ids;
    for (const auto& child : container->GetChildren())
        ids.push_back(child->GetId());
    return ids;
}
} // namespace

TEST(ToolbarDragDropSetupTests, ResetReturnsTheAuthoredOrderOfAReplacementContainer)
{
    auto root = std::make_unique<UIElement>();
    auto first = MakeResetContainer("A", "B");
    UIElement* replaced = first.get();
    root->AddChild(std::move(first));

    ToolbarDragDrop dragDrop;
    // A key no preferences file carries, so LoadButtonOrder applies no saved order and
    // ResetButtonOrder has nothing to remove or save.
    const std::vector<ToolbarDragDrop::ContainerConfig> config = {
        {kResetClass, "ui.test.toolbar.reset.order"},
    };
    dragDrop.LoadButtonOrder(root.get(), config);

    root->RemoveChild(replaced);
    auto second = MakeResetContainer("B", "A");
    UIElement* replacement = second.get();
    root->AddChild(std::move(second));
    dragDrop.LoadButtonOrder(root.get(), config);

    // The user reorders the replacement, then resets it.
    replacement->AddChild(replacement->TakeChild(replacement->GetChildren().front().get()));
    ASSERT_EQ(ChildIds(replacement), (std::vector<std::string>{"A", "B"}));
    dragDrop.ResetButtonOrder(root.get(), config);

    EXPECT_EQ(ChildIds(replacement), (std::vector<std::string>{"B", "A"}))
        << "Reset returned to the authored order of the container the reload replaced";
}
