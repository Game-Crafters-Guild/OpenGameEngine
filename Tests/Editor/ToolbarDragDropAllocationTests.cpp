// The allocation contract of ToolbarDragDrop::Setup, measured rather than argued.
//
// SceneViewToolbar::OnPostLayout calls Setup on every layout pass — the wire-once skip gate
// covers WireControls, not this — so Setup is a per-frame path for the life of the editor.
// It used to allocate ~25 times per pass against the scene-view toolbar: five for a
// by-value capture of the caller's container config (one vector block plus four std::strings
// past the 15-char MSVC SSO buffer) and one std::string temporary per button visited, built
// only so HasClass(const std::string&) could hash a 28-char literal.
//
// Behaviour tests cannot see that. A future edit that captures the config by value again, or
// reaches for HasClass(const char*), restores the whole cost while every behavioural arm
// stays green — so the claim needs an arm that fails on allocation count alone.
//
// Its own target on purpose: the measurement is a process-wide allocation window, and
// EditorTests is a shared binary of 800+ tests. This file follows the same isolation rule the
// other single-purpose targets in this directory are split out for.

#include <gtest/gtest.h>

#include "UI/Controls/Button.h"
#include "UI/ToolbarDragDrop.h"
#include "UI/UIElement.h"
#include "Memory/AllocationCountScope.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using GameEngine::Button;
using GameEngine::UIElement;
using GameEngine::Editor::ToolbarDragDrop;

namespace
{
const std::vector<ToolbarDragDrop::ContainerConfig>& Containers()
{
    // Verbatim from SceneViewToolbar: the string lengths are the point (23, 26, 19, 33 —
    // every one past the 15-char SSO buffer), so shortening them would void the arm.
    static const std::vector<ToolbarDragDrop::ContainerConfig> kConfig = {
        {"sceneview-toolbar-right", "ui.sceneview.toolbar.order"},
        {"inline-tool-buttons", "ui.sceneview.toolbar.inline.order"},
    };
    return kConfig;
}

// The real scene-view button population: 13 in sceneview-toolbar-right, 7 in
// inline-tool-buttons, matching SceneViewPanel.uxml. The per-button cost was one
// allocation each, so the count is what makes the arm sensitive.
struct ToolbarTree
{
    std::unique_ptr<UIElement> Root = std::make_unique<UIElement>();

    ToolbarTree()
    {
        AddContainer("sceneview-toolbar-right", 13);
        AddContainer("inline-tool-buttons", 7);
    }

  private:
    void AddContainer(const char* cssClass, int buttonCount)
    {
        auto container = std::make_unique<UIElement>();
        container->AddClass(cssClass);
        for (int i = 0; i < buttonCount; ++i)
        {
            auto button = std::make_unique<Button>();
            button->SetId(std::string(cssClass) + "-btn-" + std::to_string(i));
            container->AddChild(std::move(button));
        }
        Root->AddChild(std::move(container));
    }
};
} // namespace

// The headline claim: once the toolbar has settled, the per-frame Setup allocates nothing.
TEST(ToolbarDragDropAllocationTests, SettledSetupPassesAllocateNothing)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;

    // Settle: resolve the containers, mark every button, and size the row vector. The
    // first passes are allowed to allocate; the steady state is what is being pinned.
    for (int pass = 0; pass < 3; ++pass)
        dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());

    constexpr int kMeasuredPasses = 10;
    size_t allocations = 0;
    {
        const GameEngine::Memory::AllocationCountScope probe(GameEngine::Memory::CountWindow::Process);
        for (int pass = 0; pass < kMeasuredPasses; ++pass)
            dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());
        allocations = probe.Count();
    }

    EXPECT_EQ(allocations, 0u)
        << "settled ToolbarDragDrop::Setup allocated " << allocations << " times across "
        << kMeasuredPasses << " passes (" << (allocations / kMeasuredPasses)
        << "/pass). A by-value capture of the container config or a HasClass(const char*)"
           " will do this.";
}

// Positive control. A probe that never counts would make the arm above pass no matter what
// Setup does, so prove the instrument can see the allocations it is meant to catch: the
// FIRST Setup over a fresh toolbar must register some.
TEST(ToolbarDragDropAllocationTests, TheProbeSeesTheAllocationsOfAnUnsettledPass)
{
    ToolbarTree tree;
    ToolbarDragDrop dragDrop;

    size_t allocations = 0;
    {
        const GameEngine::Memory::AllocationCountScope probe(GameEngine::Memory::CountWindow::Process);
        dragDrop.Setup(tree.Root.get(), Containers(), tree.Root.get());
        allocations = probe.Count();
    }

    EXPECT_GT(allocations, 0u)
        << "the allocation probe counted nothing on a first, unsettled Setup — it is not "
           "wired up, so the zero-allocation arm above proves nothing";
}
