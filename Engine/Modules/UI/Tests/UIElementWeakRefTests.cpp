// UIElement::WeakRef — the non-owning handle a callback captures instead of a raw widget
// pointer when it can outlive the subtree it was created in.
//
// The contract is two-sided and each arm pins one side: a live element resolves to itself
// (typed, so a WeakRef<Label> hands back a Label*), and a destroyed element resolves to
// nullptr however it died — direct destruction, or a parent clearing its children the way
// an inspector rebuild does. The rebuild arm is the one that goes red on the bug the
// handle exists for: a picker's live-preview callback dereferencing the swatch a material
// rebuild had already freed.
#include <gtest/gtest.h>

#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <memory>

using namespace GameEngine;

namespace
{

TEST(UIElementWeakRef, AliveElementResolvesToItselfWithItsType)
{
    auto label = std::make_unique<Label>();
    label->SetText("alive");

    const UIElement::WeakRef<Label> ref = UIElement::MakeWeakRef(label.get());

    Label* resolved = ref.Get();
    ASSERT_EQ(resolved, label.get());
    EXPECT_EQ(resolved->GetText(), "alive");
}

TEST(UIElementWeakRef, DestroyedElementResolvesToNull)
{
    auto element = std::make_unique<UIElement>();
    const UIElement::WeakRef<> ref = UIElement::MakeWeakRef(element.get());
    ASSERT_EQ(ref.Get(), element.get());

    element.reset();

    EXPECT_EQ(ref.Get(), nullptr);
}

TEST(UIElementWeakRef, ParentClearingChildrenExpiresChildRefsOnly)
{
    auto parent = std::make_unique<UIElement>();
    auto swatch = std::make_unique<UIElement>();
    auto value = std::make_unique<Label>();
    const UIElement::WeakRef<> swatchRef = UIElement::MakeWeakRef(swatch.get());
    const UIElement::WeakRef<Label> valueRef = UIElement::MakeWeakRef(value.get());
    const UIElement::WeakRef<> parentRef = UIElement::MakeWeakRef(parent.get());
    parent->AddChild(std::move(swatch));
    parent->AddChild(std::move(value));

    parent->RemoveAllChildren();

    EXPECT_EQ(swatchRef.Get(), nullptr);
    EXPECT_EQ(valueRef.Get(), nullptr);
    EXPECT_EQ(parentRef.Get(), parent.get());
}

TEST(UIElementWeakRef, CopiesShareOneLiveness)
{
    auto element = std::make_unique<UIElement>();
    const UIElement::WeakRef<> first = UIElement::MakeWeakRef(element.get());
    const UIElement::WeakRef<> second = UIElement::MakeWeakRef(element.get());
    const UIElement::WeakRef<> copy = first;
    ASSERT_EQ(second.Get(), element.get());
    ASSERT_EQ(copy.Get(), element.get());

    element.reset();

    EXPECT_EQ(first.Get(), nullptr);
    EXPECT_EQ(second.Get(), nullptr);
    EXPECT_EQ(copy.Get(), nullptr);
}

TEST(UIElementWeakRef, DefaultAndNullSourceResolveToNull)
{
    const UIElement::WeakRef<Label> empty;
    EXPECT_EQ(empty.Get(), nullptr);

    const UIElement::WeakRef<Label> fromNull = UIElement::MakeWeakRef<Label>(nullptr);
    EXPECT_EQ(fromNull.Get(), nullptr);
}

} // namespace
