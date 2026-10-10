// The derived half of entity enable state: DisabledInHierarchy, owned by the
// pass that reads the Parent chain.
//
// An entity's own state is written where the toggle happens, in one archetype
// move. Everything below it is this system's job, and the cases that matter are
// the ones nobody writes by hand: a child that inherits, a grandchild two links
// down, a child reparented under a disabled branch after the fact, a child that
// is individually off under an enabled parent, and the cleanup when the parent
// comes back.

#include <gtest/gtest.h>

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/DisabledInHierarchySystem.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include <cstdlib>

using namespace GameEngine;
using GameEngine::Components::Parent;
using GameEngine::Components::Transform;

namespace
{
ECS::EntityHandle MakeNode(ECS::World& world, ECS::EntityHandle parent = {})
{
    ECS::Entity entity = world.Create();
    entity.Set(Transform{});
    if (parent.IsValid())
        entity.Set(Parent{parent});
    world.ProcessCommands();
    return entity.GetHandle();
}

bool IsInactive(ECS::World& world, ECS::EntityHandle entity)
{
    return !ECS::Entity(&world, entity).IsEnabledInHierarchy();
}

std::size_t VisibleTransforms(ECS::World& world)
{
    return world.Query<ECS::Read<Transform>>().Count();
}
} // namespace

TEST(DisabledInHierarchyTests, ASubtreeInheritsItsParentsState)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    const ECS::EntityHandle child = MakeNode(world, root);
    const ECS::EntityHandle grandchild = MakeNode(world, child);
    const ECS::EntityHandle sibling = MakeNode(world);

    system.Update(world, 0.0f);
    ASSERT_EQ(VisibleTransforms(world), 4u);

    ECS::Entity(&world, root).SetEnabled(false);
    system.Update(world, 0.0f);

    EXPECT_TRUE(IsInactive(world, root));
    EXPECT_TRUE(IsInactive(world, child));
    EXPECT_TRUE(IsInactive(world, grandchild));
    EXPECT_FALSE(IsInactive(world, sibling));
    EXPECT_EQ(VisibleTransforms(world), 1u);

    // The descendants' own state is untouched — they are off because of their
    // ancestor, which is what lets re-enabling the root restore exactly the
    // entities that were on before.
    EXPECT_TRUE(ECS::Entity(&world, child).IsEnabled());
    EXPECT_TRUE(ECS::Entity(&world, grandchild).IsEnabled());

    // Re-deriving must reproduce the same answer. Any later structural change
    // wakes the pass again, and by then the intermediate links are themselves
    // excluded from an ordinary query — a re-derivation that cannot see them
    // would hand their descendants back to every system.
    MakeNode(world);
    system.Update(world, 0.0f);
    EXPECT_TRUE(IsInactive(world, child));
    EXPECT_TRUE(IsInactive(world, grandchild));
    EXPECT_EQ(system.GetLastPropagatedCount(), 0u) << "a stable world owes no tag changes";

    ECS::Entity(&world, root).SetEnabled(true);
    system.Update(world, 0.0f);
    EXPECT_EQ(VisibleTransforms(world), 5u);
}

TEST(DisabledInHierarchyTests, AChildDisabledOnItsOwnStaysOffWhenTheParentComesBack)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    const ECS::EntityHandle keptOff = MakeNode(world, root);
    const ECS::EntityHandle restored = MakeNode(world, root);

    ECS::Entity(&world, keptOff).SetEnabled(false);
    ECS::Entity(&world, root).SetEnabled(false);
    system.Update(world, 0.0f);
    EXPECT_EQ(VisibleTransforms(world), 0u);

    ECS::Entity(&world, root).SetEnabled(true);
    system.Update(world, 0.0f);

    EXPECT_FALSE(IsInactive(world, root));
    EXPECT_FALSE(IsInactive(world, restored));
    EXPECT_TRUE(IsInactive(world, keptOff)) << "its own state survives the parent's";
    EXPECT_EQ(VisibleTransforms(world), 2u);
}

TEST(DisabledInHierarchyTests, AChildSwitchedBackOnUnderADisabledParentStaysInactiveBeforeThePass)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    const ECS::EntityHandle child = MakeNode(world, root);

    ECS::Entity(&world, child).SetEnabled(false);
    ECS::Entity(&world, root).SetEnabled(false);
    system.Update(world, 0.0f);

    // Between two passes, where an editor toggle or a script switches it: the
    // pass named Parent as the world's parent relation when it bound, so the
    // switch itself keeps the derived tag while the parent is off.
    ECS::Entity(&world, child).SetEnabled(true);
    EXPECT_TRUE(ECS::Entity(&world, child).IsEnabled());
    EXPECT_TRUE(IsInactive(world, child));
    EXPECT_EQ(VisibleTransforms(world), 0u);
}

TEST(DisabledInHierarchyTests, GrandparentOnParentStaleChildOnErrsHiddenForExactlyOnePass)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;
    const ECS::EntityHandle grand = MakeNode(world);
    const ECS::EntityHandle parent = MakeNode(world, grand);
    const ECS::EntityHandle child = MakeNode(world, parent);

    ECS::Entity(&world, child).SetEnabled(false);
    ECS::Entity(&world, grand).SetEnabled(false);
    system.Update(world, 0.0f);
    ASSERT_TRUE(world.HasComponent<ECS::DisabledInHierarchy>(parent));
    ASSERT_FALSE(world.HasComponent<ECS::Disabled>(parent));

    ECS::Entity(&world, grand).SetEnabled(true); // the parent's derived tag is now stale
    ECS::Entity(&world, child).SetEnabled(true);
    EXPECT_FALSE(IsInactive(world, grand));
    EXPECT_TRUE(IsInactive(world, parent)) << "stale until the pass";
    EXPECT_TRUE(IsInactive(world, child)) << "the lookup errs towards hidden";

    const std::size_t derivations = system.GetDerivationCount();
    system.Update(world, 0.0f);
    EXPECT_EQ(system.GetDerivationCount(), derivations + 1) << "the switch bumped the structural version";
    EXPECT_FALSE(IsInactive(world, parent));
    EXPECT_FALSE(IsInactive(world, child)) << "one pass corrects it";
    EXPECT_EQ(VisibleTransforms(world), 3u);
}

TEST(DisabledInHierarchyTests, ReparentingUnderADisabledBranchInheritsIt)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle offBranch = MakeNode(world);
    const ECS::EntityHandle onBranch = MakeNode(world);
    const ECS::EntityHandle mover = MakeNode(world, onBranch);

    ECS::Entity(&world, offBranch).SetEnabled(false);
    system.Update(world, 0.0f);
    EXPECT_FALSE(IsInactive(world, mover));

    // A reparent writes an existing Parent column, which is a data write and
    // bumps no structural version: the pass has to notice it through the
    // change filter.
    *world.GetComponentForWrite<Parent>(mover) = Parent{offBranch};
    system.Update(world, 0.0f);
    EXPECT_TRUE(IsInactive(world, mover));

    *world.GetComponentForWrite<Parent>(mover) = Parent{onBranch};
    system.Update(world, 0.0f);
    EXPECT_FALSE(IsInactive(world, mover));
}

TEST(DisabledInHierarchyTests, AnEntityCreatedUnderADisabledParentIsInactiveToo)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    ECS::Entity(&world, root).SetEnabled(false);
    system.Update(world, 0.0f);

    const ECS::EntityHandle late = MakeNode(world, root);
    system.Update(world, 0.0f);
    EXPECT_TRUE(IsInactive(world, late));
}

TEST(DisabledInHierarchyTests, ATagAddedDirectlyIsStillPropagated)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    const ECS::EntityHandle child = MakeNode(world, root);

    // What scene load and the debug server do: the authored tag alone. The
    // entity is already excluded by it; the pass owes the subtree.
    world.AddComponentImmediate<ECS::Disabled>(root, ECS::Disabled{});
    EXPECT_EQ(VisibleTransforms(world), 1u) << "the authored tag excludes on its own";

    system.Update(world, 0.0f);
    EXPECT_TRUE(IsInactive(world, child));
    EXPECT_EQ(VisibleTransforms(world), 0u);
}

TEST(DisabledInHierarchyTests, ACycleInTheParentChainTerminates)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle first = MakeNode(world);
    const ECS::EntityHandle second = MakeNode(world, first);
    world.AddComponentImmediate<Parent>(first, Parent{second});
    const ECS::EntityHandle unrelated = MakeNode(world);
    ECS::Entity(&world, unrelated).SetEnabled(false);

    system.Update(world, 0.0f);

    EXPECT_FALSE(IsInactive(world, first));
    EXPECT_FALSE(IsInactive(world, second));
    EXPECT_TRUE(IsInactive(world, unrelated));
}

TEST(DisabledInHierarchyTests, AWorldWithNothingDisabledNeverDerives)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    MakeNode(world, root);
    MakeNode(world, root);

    // Structural changes wake the pass; with nothing disabled it exits on two
    // archetype-level counts before it derives anything.
    for (int frame = 0; frame < 4; ++frame)
    {
        MakeNode(world, root);
        system.Update(world, 0.0f);
    }
    EXPECT_EQ(system.GetDerivationCount(), 0u);
}

TEST(DisabledInHierarchyTests, AnIdleWorldWithDisabledEntitiesDerivesOnlyWhenSomethingChanged)
{
    ECS::World world(nullptr);
    ECS::DisabledInHierarchySystem system;

    const ECS::EntityHandle root = MakeNode(world);
    MakeNode(world, root);
    MakeNode(world, root);
    system.Update(world, 0.0f);

    ECS::Entity(&world, root).SetEnabled(false);
    system.Update(world, 0.0f);
    ASSERT_EQ(system.GetLastPropagatedCount(), 2u);
    const std::size_t derivations = system.GetDerivationCount();

    // The pass's own moves stamped the chunks it touched, Parent column
    // included; that must not read as a reparent on the next update.
    system.Update(world, 0.0f);
    system.Update(world, 0.0f);
    EXPECT_EQ(system.GetDerivationCount(), derivations)
        << "an idle world with disabled entities re-derived after its own propagation";

    // A real reparent still does.
    const ECS::EntityHandle onBranch = MakeNode(world);
    const ECS::EntityHandle mover = MakeNode(world, onBranch);
    system.Update(world, 0.0f);
    const std::size_t afterSpawn = system.GetDerivationCount();
    *world.GetComponentForWrite<Parent>(mover) = Parent{root};
    system.Update(world, 0.0f);
    EXPECT_EQ(system.GetDerivationCount(), afterSpawn + 1);
    EXPECT_FALSE(ECS::Entity(&world, mover).IsEnabledInHierarchy());
}

#if !defined(NDEBUG)
static void QuietCrtInDeathTestChild()
{
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}

// A test process that cleared the registry and then runs the pass on a world with
// no Parent anywhere: BindTo registers Parent itself, or SetParentRelation asserts.
TEST(DisabledInHierarchyTests, ThePassBindsAfterTheRegistryWasCleared)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            QuietCrtInDeathTestChild();
            ECS::ComponentRegistry::Clear();
            ECS::World world(nullptr);
            ECS::DisabledInHierarchySystem system;
            system.Update(world, 0.0f);
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}
#endif
