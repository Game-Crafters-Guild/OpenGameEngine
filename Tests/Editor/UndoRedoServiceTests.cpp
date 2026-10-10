#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "EditorChangeNotifications.h"
#include "EditorContextMenu/InterceptableContextMenu.h"
#include "Input/KeyCodes.h"
#include "InspectorRegistry.h"
#include "Editor/Entities/ComponentEnabledToggle.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Inspectors/AnimatorInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/TransformInspector.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Animation/Animator.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Audio/AudioListener.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Transform.h"
#include "Components/Hierarchy.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Vector3Field.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;

using GameEngine::ECS::Entity;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;

using GameEngine::Components::Transform;
using GameEngine::Components::Parent;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

using GameEngine::Editor::UndoRedoService;

using GameEngine::FloatField;
using GameEngine::InspectorContext;
using GameEngine::InspectorFn;
using GameEngine::InspectorRegistry;
using GameEngine::Slider;
using GameEngine::TextInput;
using GameEngine::UIElement;
using GameEngine::Vector3Field;

namespace
{

static void MultiplyMat4ColumnMajor(const float* a, const float* b, float* out)
{
    // Column-major 4x4 multiply: out = a * b
    // m[col*4 + row]
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

static std::array<float, 16> ComputeWorldMatrix(World* world, EntityHandle entity)
{
    std::array<float, 16> out{};
    // identity
    for (int i = 0; i < 16; ++i) out[(size_t)i] = 0.0f;
    out[0] = out[5] = out[10] = out[15] = 1.0f;

    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return out;

    // Accumulate by walking up the Parent chain, pre-multiplying ancestor local transforms.
    EntityHandle cur = entity;

    // Start with local(cur)
    auto* t0 = world->GetComponent<Transform>(cur);
    if (!t0)
        return out;
    for (int i = 0; i < 16; ++i) out[(size_t)i] = t0->matrix[i];

    while (true)
    {
        auto* p = world->GetComponent<Parent>(cur);
        if (!p || !p->parent.IsValid() || !world->IsValid(p->parent))
            break;

        auto* pt = world->GetComponent<Transform>(p->parent);
        if (!pt)
            break;

        std::array<float, 16> tmp{};
        MultiplyMat4ColumnMajor(pt->matrix, out.data(), tmp.data());
        out = tmp;
        cur = p->parent;
    }

    return out;
}

static void ExpectMat4Near(const std::array<float, 16>& a, const std::array<float, 16>& b, float eps = 1.0e-5f)
{
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_NEAR(a[(size_t)i], b[(size_t)i], eps);
    }
}

// The Transform inspector lays its rows out in build order: Local Position,
// Local Rotation, Local Scale, World Position.
static std::vector<Vector3Field*> CollectVector3Fields(UIElement* node)
{
    std::vector<Vector3Field*> out;
    auto walk = [&out](UIElement* n, const auto& self) -> void
    {
        if (!n)
            return;
        if (auto* v = dynamic_cast<Vector3Field*>(n))
        {
            out.push_back(v);
            return;
        }
        for (const auto& ch : n->GetChildren())
            self(ch.get(), self);
    };
    walk(node, walk);
    return out;
}

// The row's draggable name label ("Local Position"), which is where the reset
// double-click and the row context menu are wired — not the X/Y/Z axis labels
// inside the Vector3Field.
static GameEngine::Label* FindRowLabelFor(UIElement* node, Vector3Field* field)
{
    if (!node || !field)
        return nullptr;

    bool holdsField = false;
    for (const auto& ch : node->GetChildren())
        holdsField = holdsField || (ch.get() == field);

    if (holdsField)
    {
        for (const auto& ch : node->GetChildren())
        {
            if (auto* label = dynamic_cast<GameEngine::Label*>(ch.get()))
                return label;
        }
        return nullptr;
    }

    for (const auto& ch : node->GetChildren())
    {
        if (auto* found = FindRowLabelFor(ch.get(), field))
            return found;
    }
    return nullptr;
}

// The interceptor is a process-wide static, so it must come back down however a
// test leaves — including through a failed ASSERT — or every later menu in the
// binary stays swallowed.
class InterceptorGuard
{
public:
    explicit InterceptorGuard(GameEngine::InterceptableContextMenu::Interceptor interceptor)
    {
        GameEngine::InterceptableContextMenu::SetInterceptor(std::move(interceptor));
    }
    InterceptorGuard(const InterceptorGuard&) = delete;
    InterceptorGuard& operator=(const InterceptorGuard&) = delete;
    ~InterceptorGuard() { GameEngine::InterceptableContextMenu::SetInterceptor(nullptr); }
};

// The dispatch the UI module's own control tests use: hand the element an event
// and let its registered handlers run.
static void SendMouse(UIElement& el, GameEngine::EventId id, int button = 0)
{
    GameEngine::UIEvent e{};
    e.Id = id;
    e.X = 0.0f;
    e.Y = 0.0f;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

class UndoRedoServiceTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    World* GetWorld() const
    {
        return EngineCore::GetInstance().EnsurePrimaryWorld();
    }

    void SetUp() override
    {
        World* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
    }
};

TEST_F(UndoRedoServiceTests, InteractiveEditCoalescesToSingleUndoCommand)
{
    UndoRedoService svc;

    int value = 0;
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = "Int Value";
    target.Capture = [&value](UndoRedoService::SnapshotTarget::Snapshot& out)
    {
        out.resize(sizeof(int));
        std::memcpy(out.data(), &value, sizeof(int));
        return true;
    };
    target.Apply = [&value](const UndoRedoService::SnapshotTarget::Snapshot& snap)
    {
        if (snap.size() != sizeof(int))
            return false;
        std::memcpy(&value, snap.data(), sizeof(int));
        return true;
    };

    auto edit = svc.BeginInteractiveEdit("Value Scrub", target);
    ASSERT_TRUE((bool)edit);

    // Preview multiple times; no command should be pushed until Commit.
    edit.Preview([&]() { value = 1; });
    edit.Preview([&]() { value = 2; });
    EXPECT_EQ(svc.GetUndoCount(), 0u);
    EXPECT_EQ(svc.GetRedoCount(), 0u);

    edit.Commit();

    EXPECT_EQ(value, 2);
    EXPECT_EQ(svc.GetUndoCount(), 1u);
    EXPECT_EQ(svc.GetRedoCount(), 0u);

    svc.Undo();
    EXPECT_EQ(value, 0);
    EXPECT_EQ(svc.GetUndoCount(), 0u);
    EXPECT_EQ(svc.GetRedoCount(), 1u);

    svc.Redo();
    EXPECT_EQ(value, 2);
    EXPECT_EQ(svc.GetUndoCount(), 1u);
    EXPECT_EQ(svc.GetRedoCount(), 0u);
}

// The section enable dot on a multi-selection switches the component on every selected entity, in
// one undo step that puts each entity's own switch back. The switch is the component's
// ECS::ComponentDisabled tag, which the component's bytes do not carry, so the step snapshots it. A
// selection that disagrees reads as mixed until the click switches it to one state. The click is
// announced as a commit and its undo as UndoRedo, the kind that makes the inspector redraw the dot.
TEST_F(UndoRedoServiceTests, ComponentEnabledToggleSwitchesTheWholeSelectionInOneUndoStep)
{
    World* world = GetWorld();
    const GameEngine::ECS::ComponentTypeId typeId =
        GameEngine::ECS::GetComponentTypeId<GameEngine::Components::AudioEmitter>();
    const EntityHandle primary = world->CreateEntity();
    const EntityHandle peerOn = world->CreateEntity();
    const EntityHandle peerOff = world->CreateEntity();
    const std::vector<EntityHandle> selection{primary, peerOn, peerOff};
    for (EntityHandle entity : selection)
        world->AddComponentImmediate(entity, GameEngine::Components::AudioEmitter{});
    world->SetComponentEnabledImmediate(peerOff, typeId, false);
    ASSERT_TRUE(GameEngine::Editor::IsComponentEnabledMixed(world, selection, typeId));

    using ChangeKind = GameEngine::Editor::EditorChangeNotifications::ChangeKind;
    GameEngine::Editor::EditorChangeNotifications notifications;
    std::vector<ChangeKind> announced;
    const auto subscription = notifications.SubscribeComponentChanged(
        [&announced](const GameEngine::Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        { announced.push_back(e.kind); });

    UndoRedoService svc;
    GameEngine::Editor::CommitComponentEnabledToggle(*world, &svc, &notifications, primary, selection, typeId,
                                                     "Audio Emitter", false);
    EXPECT_EQ(announced, std::vector<ChangeKind>(3, ChangeKind::Commit));
    ASSERT_EQ(svc.GetUndoCount(), 1u) << "one click is one undo step for the whole selection";
    EXPECT_STREQ(svc.PeekUndoName(), "Switch Audio Emitter off (3 entities)")
        << "the step names the component, the direction and how many entities it switched";
    for (EntityHandle entity : selection)
        EXPECT_FALSE(world->IsComponentEnabled(entity, typeId)) << "every selected emitter switches off";
    EXPECT_FALSE(GameEngine::Editor::IsComponentEnabledMixed(world, selection, typeId));

    announced.clear();
    svc.Undo();
    EXPECT_TRUE(world->IsComponentEnabled(primary, typeId));
    EXPECT_TRUE(world->IsComponentEnabled(peerOn, typeId));
    EXPECT_FALSE(world->IsComponentEnabled(peerOff, typeId)) << "undo puts each entity's own switch back";
    EXPECT_EQ(announced, std::vector<ChangeKind>(3, ChangeKind::UndoRedo)) << "the inspector redraws on undo";
    notifications.Unsubscribe(subscription);
    svc.Redo();
    for (EntityHandle entity : selection)
        EXPECT_FALSE(world->IsComponentEnabled(entity, typeId));
}

namespace
{
// Components no production code registers editor traits for, so the tooltip test owns their entries
// in the process-wide registry without replacing a real component's traits for the tests after it.
struct ToggleTooltipProbe
{
    int Value = 0;
};
struct HostedToggleTooltipProbe
{
    int Value = 0;
};
struct ToggleTooltipProbeHost
{
    int Value = 0;
};
} // namespace

// The section enable dot's tooltip is the one the component's owner registered for its type, or,
// for a type that registers none, the one the component hosting its section registered for its
// entries, each replacing the generic tooltip; a selection that disagrees adds which way a click
// switches them all: the opposite of the primary entity's state.
TEST_F(UndoRedoServiceTests, ComponentEnabledToggleTooltipIsTheOwnersWhenItRegistersOne)
{
    namespace Components = GameEngine::Components;
    namespace Editor = GameEngine::Editor;
    using GameEngine::ECS::GetComponentTypeId;
    World* world = GetWorld();
    const GameEngine::ECS::ComponentTypeId probeId = GetComponentTypeId<ToggleTooltipProbe>();
    const EntityHandle probeOn = world->CreateEntity();
    const EntityHandle probeOff = world->CreateEntity();
    world->AddComponentImmediate(probeOn, ToggleTooltipProbe{});
    world->AddComponentImmediate(probeOff, ToggleTooltipProbe{});
    world->SetComponentEnabledImmediate(probeOff, probeId, false);
    const std::vector<EntityHandle> mixed{probeOn, probeOff};

    const std::string generic =
        Editor::ComponentEnabledToggleTooltip(world, probeOn, {}, GetComponentTypeId<Components::AudioListener>());
    ASSERT_FALSE(generic.empty());

    Editor::EditorComponentTraits traits;
    traits.EnableToggleTooltip = "Switch the probe on or off. Off stops its voice.";
    Editor::EditorComponentTraitsRegistry::Get().Register(probeId, std::move(traits));
    EXPECT_EQ(Editor::ComponentEnabledToggleTooltip(world, probeOn, {}, probeId),
              "Switch the probe on or off. Off stops its voice.");
    EXPECT_EQ(Editor::ComponentEnabledToggleTooltip(world, probeOn, mixed, probeId),
              "Switch the probe on or off. Off stops its voice. The selected entities disagree; a click switches "
              "all of them off.");
    EXPECT_EQ(Editor::ComponentEnabledToggleTooltip(world, probeOff, mixed, probeId),
              "Switch the probe on or off. Off stops its voice. The selected entities disagree; a click switches "
              "all of them on.");

    const GameEngine::ECS::ComponentTypeId hostedId = GetComponentTypeId<HostedToggleTooltipProbe>();
    const EntityHandle hosted = world->CreateEntity();
    world->AddComponentImmediate(hosted, HostedToggleTooltipProbe{});
    Editor::EditorComponentTraits hostTraits;
    hostTraits.HostsInspectorSection = [hostedId](GameEngine::ECS::ComponentTypeId typeId)
    { return typeId == hostedId; };
    hostTraits.HostedEnableToggleTooltip = "Switch this effect on or off. Off leaves the probe host alone.";
    Editor::EditorComponentTraitsRegistry::Get().Register(GetComponentTypeId<ToggleTooltipProbeHost>(),
                                                          std::move(hostTraits));
    EXPECT_EQ(Editor::ComponentEnabledToggleTooltip(world, hosted, {}, hostedId),
              "Switch this effect on or off. Off leaves the probe host alone.");
}

// A dot click that switches nothing records no undo step: a type with no switch (Transform), or an
// entity that already has the clicked state.
TEST_F(UndoRedoServiceTests, AComponentDotClickThatSwitchesNothingRecordsNoUndoStep)
{
    namespace Components = GameEngine::Components;
    using GameEngine::ECS::GetComponentTypeId;
    World* world = GetWorld();
    const EntityHandle entity = world->CreateEntity();
    world->AddComponentImmediate(entity, Components::Transform{});
    world->AddComponentImmediate(entity, Components::AudioEmitter{});

    UndoRedoService svc;
    GameEngine::Editor::CommitComponentEnabledToggle(*world, &svc, nullptr, entity, {},
                                                     GetComponentTypeId<Components::Transform>(), "Transform", false);
    GameEngine::Editor::CommitComponentEnabledToggle(*world, &svc, nullptr, entity, {},
                                                     GetComponentTypeId<Components::AudioEmitter>(), "Audio Emitter",
                                                     true);
    EXPECT_EQ(svc.GetUndoCount(), 0u);
}

// The Animator's dot is its switch, like every other component's: it writes the Animator's
// ECS::ComponentDisabled tag and leaves autoPlayOnEnterPlayMode, which is a row of its own.
TEST_F(UndoRedoServiceTests, TheAnimatorDotSwitchesTheAnimatorAndLeavesAutoPlay)
{
    namespace Components = GameEngine::Components;
    World* world = GetWorld();
    const EntityHandle entity = world->CreateEntity();
    world->AddComponentImmediate(entity, Components::Animator{});
    const GameEngine::ECS::ComponentTypeId typeId = GameEngine::ECS::GetComponentTypeId<Components::Animator>();
    ASSERT_TRUE(world->GetComponent<Components::Animator>(entity)->autoPlayOnEnterPlayMode);

    UndoRedoService svc;
    GameEngine::Editor::CommitComponentEnabledToggle(*world, &svc, nullptr, entity, {}, typeId, "Animator", false);
    EXPECT_FALSE(world->IsComponentEnabled(entity, typeId)) << "the dot switches the Animator itself";
    EXPECT_TRUE(world->GetComponent<Components::Animator>(entity)->autoPlayOnEnterPlayMode)
        << "auto-play is its own row, untouched by the dot";
    EXPECT_STREQ(svc.PeekUndoName(), "Switch Animator off");
    GameEngine::RegisterAnimatorInspector();
    EXPECT_EQ(GameEngine::Editor::ComponentEnabledToggleTooltip(world, entity, {}, typeId),
              "Switch this component on or off. Off keeps its animation from playing; auto-play is the row below.")
        << "the tooltip says the dot no longer stands for auto-play";
}

// A terrain effect keeps its own Enabled field for the bake to read
// (ECS::ComponentFlags::KeepsOwnEnabledField). Its section dot edits that field in one undo step
// named after the effect, and writes no ComponentDisabled tag the bake would not see (#2094: the
// Enabled row this dot replaces recorded no undo step).
TEST_F(UndoRedoServiceTests, ATerrainEffectDotEditsItsEnabledFieldInOneUndoStep)
{
    using GameEngine::Components::TerrainFlattenEffect;
    World* world = GetWorld();
    const EntityHandle entity = world->CreateEntity();
    world->AddComponentImmediate(entity, TerrainFlattenEffect{});
    const GameEngine::ECS::ComponentTypeId typeId = GameEngine::ECS::GetComponentTypeId<TerrainFlattenEffect>();
    ASSERT_FALSE(GameEngine::ECS::ComponentRegistry::SwitchesThroughDisabledTag(typeId));

    bool shown = false;
    ASSERT_TRUE(GameEngine::Editor::TryGetComponentEnabled(world, entity, typeId, shown)) << "the effect has a dot";
    EXPECT_TRUE(shown);

    UndoRedoService svc;
    GameEngine::Editor::CommitComponentEnabledToggle(*world, &svc, nullptr, entity, {}, typeId, "Flatten", false);
    ASSERT_EQ(svc.GetUndoCount(), 1u);
    EXPECT_STREQ(svc.PeekUndoName(), "Switch Flatten off");
    EXPECT_FALSE(world->GetComponent<TerrainFlattenEffect>(entity)->Enabled);
    EXPECT_TRUE(world->IsComponentEnabled(entity, typeId)) << "no tag: the bake reads the field";

    svc.Undo();
    EXPECT_TRUE(world->GetComponent<TerrainFlattenEffect>(entity)->Enabled);
    svc.Redo();
    EXPECT_FALSE(world->GetComponent<TerrainFlattenEffect>(entity)->Enabled);
}

TEST_F(UndoRedoServiceTests, TransformSnapshotUndoRedoRestoresExactLocalPosition)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    Entity e = world->Create();

    Vector3 startPos(1.0f, 2.0f, 3.0f);
    Quaternion rot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 scale(1.0f, 1.0f, 1.0f);
    Transform start = Transform::FromTRS(startPos, rot, scale);
    e.Set(start);
    world->ProcessCommands();

    EntityHandle handle = e.GetHandle();
    const auto typeId = GameEngine::ECS::GetComponentTypeId<Transform>();

    UndoRedoService svc;
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = "Transform";
    target.Capture = [world, handle, typeId](UndoRedoService::SnapshotTarget::Snapshot& out)
    {
        return world->CaptureComponentBytes(handle, typeId, out);
    };
    target.Apply = [world, handle, typeId](const UndoRedoService::SnapshotTarget::Snapshot& snap)
    {
        return world->ApplyComponentBytesImmediate(handle, typeId, snap);
    };

    auto edit = svc.BeginInteractiveEdit("Transform Translate", target);
    ASSERT_TRUE((bool)edit);

    Vector3 endPos(4.0f, -1.0f, 0.25f);
    edit.Preview([&]()
                 {
                     // Apply a new transform in the same way the editor does: immediate set.
                     Transform updated = Transform::FromTRS(endPos, rot, scale);
                     world->AddComponentImmediate(handle, updated);
                 });

    edit.Commit();

    {
        auto* t = world->GetComponent<Transform>(handle);
        ASSERT_NE(t, nullptr);
        Vector3 p =t->GetPosition();
        EXPECT_NEAR(p.x, endPos.x, 1.0e-5f);
        EXPECT_NEAR(p.y, endPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, endPos.z, 1.0e-5f);
    }

    svc.Undo();
    {
        auto* t = world->GetComponent<Transform>(handle);
        ASSERT_NE(t, nullptr);
        Vector3 p =t->GetPosition();
        EXPECT_NEAR(p.x, startPos.x, 1.0e-5f);
        EXPECT_NEAR(p.y, startPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, startPos.z, 1.0e-5f);
    }

    svc.Redo();
    {
        auto* t = world->GetComponent<Transform>(handle);
        ASSERT_NE(t, nullptr);
        Vector3 p =t->GetPosition();
        EXPECT_NEAR(p.x, endPos.x, 1.0e-5f);
        EXPECT_NEAR(p.y, endPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, endPos.z, 1.0e-5f);
    }
}

TEST_F(UndoRedoServiceTests, TransformInspectorEditPreviewsLiveAndCommitsOnce)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    // Ensure the Transform inspector is registered for this test run.
    GameEngine::RegisterTransformInspector();

    Entity e = world->Create();
    Vector3 startPos(0.0f, 0.0f, 0.0f);
    Quaternion rot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 scale(1.0f, 1.0f, 1.0f);
    Transform start = Transform::FromTRS(startPos, rot, scale);
    e.Set(start);
    world->ProcessCommands();

    EntityHandle handle = e.GetHandle();
    auto* t = world->GetComponentForWrite<Transform>(handle);
    ASSERT_NE(t, nullptr);

    // Build the inspector UI subtree into a lightweight root element.
    UIElement root("TestRoot");
    UndoRedoService undo;

    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.World = world;
    ctx.Entity = handle;
    ctx.Object = t;
    ctx.Undo = &undo;
    ctx.ChangeNotifications = nullptr; // keep test focused on preview/commit + undo stack

    InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Transform>();
    ASSERT_NE(inspector, nullptr);
    (*inspector)(ctx);

    // Find the first Vector3Field (Local Position) and its X component editor.
    auto findFirstVector3Field = [&](UIElement* node, const auto& self) -> Vector3Field*
    {
        if (!node)
            return nullptr;
        if (auto* v = dynamic_cast<Vector3Field*>(node))
            return v;
        for (const auto& ch : node->GetChildren())
        {
            if (auto* found = self(ch.get(), self))
                return found;
        }
        return nullptr;
    };

    Vector3Field* localPosField = findFirstVector3Field(&root, findFirstVector3Field);
    ASSERT_NE(localPosField, nullptr);

    FloatField* xField = nullptr;
    for (const auto& ch : localPosField->GetChildren())
    {
        if (auto* f = dynamic_cast<FloatField*>(ch.get()))
        {
            xField = f;
            break;
        }
    }
    ASSERT_NE(xField, nullptr);

    TextInput* editor = nullptr;
    for (const auto& ch : xField->GetChildren())
    {
        if (auto* ti = dynamic_cast<TextInput*>(ch.get()))
        {
            editor = ti;
            break;
        }
    }
    ASSERT_NE(editor, nullptr);

    // Begin editing the X component. While typing (preview), no undo entries should be pushed.
    editor->OnFocusChanged(true);
    editor->OnKey(GameEngine::Input::kKeyCode_A, 0x0002 /*GLFW_MOD_CONTROL*/, nullptr); // Ctrl+A
    editor->OnChar(static_cast<unsigned int>('2'));
    editor->OnChar(static_cast<unsigned int>('0')); // now "20"

    EXPECT_EQ(undo.GetUndoCount(), 0u);
    EXPECT_EQ(undo.GetRedoCount(), 0u);

    // Preview should have applied live to ECS.
    {
        auto* cur = world->GetComponent<Transform>(handle);
        ASSERT_NE(cur, nullptr);
        Vector3 p =cur->GetPosition();
        EXPECT_NEAR(p.x, 20.0f, 1.0e-5f);
        EXPECT_NEAR(p.y, startPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, startPos.z, 1.0e-5f);
    }

    // Commit via Enter. This should create exactly one undo step.
    editor->OnKey(257 /*GLFW_KEY_ENTER*/, 0, nullptr);

    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_EQ(undo.GetRedoCount(), 0u);
    EXPECT_STREQ(undo.PeekUndoName(), "Transform Position");

    undo.Undo();
    {
        auto* cur = world->GetComponent<Transform>(handle);
        ASSERT_NE(cur, nullptr);
        Vector3 p =cur->GetPosition();
        EXPECT_NEAR(p.x, startPos.x, 1.0e-5f);
        EXPECT_NEAR(p.y, startPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, startPos.z, 1.0e-5f);
    }

    undo.Redo();
    {
        auto* cur = world->GetComponent<Transform>(handle);
        ASSERT_NE(cur, nullptr);
        Vector3 p =cur->GetPosition();
        EXPECT_NEAR(p.x, 20.0f, 1.0e-5f);
        EXPECT_NEAR(p.y, startPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, startPos.z, 1.0e-5f);
    }
}

// Reset lands every selected entity on the row default (scale 1, not 0) and is a
// single undo step that restores each entity's own prior value — a value edit
// offsets peers by the primary's delta, which would leave them scattered around
// the default instead of on it.
TEST_F(UndoRedoServiceTests, TransformInspectorResetRowRestoresDefaultsAcrossSelectionAndUndoesOnce)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::RegisterTransformInspector();

    const Quaternion rot(1.0f, 0.0f, 0.0f, 0.0f);
    Entity primary = world->Create();
    Entity peer = world->Create();
    primary.Set(Transform::FromTRS(Vector3(3.0f, 4.0f, 5.0f), rot, Vector3(2.0f, 2.0f, 2.0f)));
    peer.Set(Transform::FromTRS(Vector3(-1.0f, 0.0f, 7.0f), rot, Vector3(5.0f, 5.0f, 5.0f)));
    world->ProcessCommands();

    const EntityHandle primaryHandle = primary.GetHandle();
    const EntityHandle peerHandle = peer.GetHandle();
    auto* primaryTransform = world->GetComponentForWrite<Transform>(primaryHandle);
    ASSERT_NE(primaryTransform, nullptr);

    UIElement root("TestRoot");
    UndoRedoService undo;

    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.World = world;
    ctx.Entity = primaryHandle;
    ctx.Entities = {primaryHandle, peerHandle};
    ctx.Object = primaryTransform;
    ctx.Undo = &undo;
    ctx.ChangeNotifications = nullptr;

    InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Transform>();
    ASSERT_NE(inspector, nullptr);
    (*inspector)(ctx);

    // Rows in build order: Local Position, Local Rotation, Local Scale, World Position.
    std::vector<Vector3Field*> fields = CollectVector3Fields(&root);
    ASSERT_GE(fields.size(), 4u);

    GameEngine::Label* scaleLabel = FindRowLabelFor(&root, fields[2]);
    ASSERT_NE(scaleLabel, nullptr);

    // Double-click the row label: press, release, press again inside the interval.
    SendMouse(*scaleLabel, GameEngine::kEventMouseDown, 0);
    SendMouse(*scaleLabel, GameEngine::kEventMouseUp, 0);
    SendMouse(*scaleLabel, GameEngine::kEventMouseDown, 0);

    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Reset Local Scale");

    {
        const Vector3 p = world->GetComponent<Transform>(primaryHandle)->GetScale();
        const Vector3 s = world->GetComponent<Transform>(peerHandle)->GetScale();
        EXPECT_NEAR(p.x, 1.0f, 1.0e-5f);
        EXPECT_NEAR(p.y, 1.0f, 1.0e-5f);
        EXPECT_NEAR(p.z, 1.0f, 1.0e-5f);
        EXPECT_NEAR(s.x, 1.0f, 1.0e-5f);
        EXPECT_NEAR(s.y, 1.0f, 1.0e-5f);
        EXPECT_NEAR(s.z, 1.0f, 1.0e-5f);
    }

    undo.Undo();
    {
        const Vector3 p = world->GetComponent<Transform>(primaryHandle)->GetScale();
        const Vector3 s = world->GetComponent<Transform>(peerHandle)->GetScale();
        EXPECT_NEAR(p.x, 2.0f, 1.0e-5f);
        EXPECT_NEAR(s.x, 5.0f, 1.0e-5f);
    }

    undo.Redo();
    {
        const Vector3 p = world->GetComponent<Transform>(primaryHandle)->GetScale();
        const Vector3 s = world->GetComponent<Transform>(peerHandle)->GetScale();
        EXPECT_NEAR(p.x, 1.0f, 1.0e-5f);
        EXPECT_NEAR(s.x, 1.0f, 1.0e-5f);
    }

    // Position resets to zero, so the default is per-row, not a blanket zero-fill.
    GameEngine::Label* posLabel = FindRowLabelFor(&root, fields[0]);
    ASSERT_NE(posLabel, nullptr);
    SendMouse(*posLabel, GameEngine::kEventMouseDown, 0);
    SendMouse(*posLabel, GameEngine::kEventMouseUp, 0);
    SendMouse(*posLabel, GameEngine::kEventMouseDown, 0);

    {
        const Vector3 p = world->GetComponent<Transform>(primaryHandle)->GetPosition();
        const Vector3 s = world->GetComponent<Transform>(peerHandle)->GetPosition();
        EXPECT_NEAR(p.x, 0.0f, 1.0e-5f);
        EXPECT_NEAR(p.z, 0.0f, 1.0e-5f);
        EXPECT_NEAR(s.x, 0.0f, 1.0e-5f);
        EXPECT_NEAR(s.z, 0.0f, 1.0e-5f);
    }
}

// The menu model itself: right-clicking a row label has to offer Reset next to
// Copy/Paste, and the command it carries has to reach the reset. Driven through
// InterceptableContextMenu's interceptor, which every editor menu rides — Show()
// publishes the resolved item tree plus a copy of the command handler instead of
// opening a popup.
TEST_F(UndoRedoServiceTests, TransformInspectorRowMenuOffersResetAndItsCommandResets)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::RegisterTransformInspector();

    const Quaternion rot(1.0f, 0.0f, 0.0f, 0.0f);
    Entity primary = world->Create();
    primary.Set(Transform::FromTRS(Vector3(0.0f, 0.0f, 0.0f), rot, Vector3(2.0f, 2.0f, 2.0f)));
    world->ProcessCommands();

    const EntityHandle handle = primary.GetHandle();
    auto* transform = world->GetComponentForWrite<Transform>(handle);
    ASSERT_NE(transform, nullptr);

    UIElement root("TestRoot");
    UndoRedoService undo;

    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.World = world;
    ctx.Entity = handle;
    ctx.Object = transform;
    ctx.Undo = &undo;
    ctx.ChangeNotifications = nullptr;
    // The row menu is gated on a window, and Show() hands it straight to the
    // interceptor without dereferencing it. A sentinel is enough to open the
    // path; nothing in this test touches a real window.
    ctx.Window = reinterpret_cast<GameEngine::Platform::Window*>(std::uintptr_t{1});

    InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Transform>();
    ASSERT_NE(inspector, nullptr);
    (*inspector)(ctx);

    std::vector<Vector3Field*> fields = CollectVector3Fields(&root);
    ASSERT_GE(fields.size(), 4u);
    GameEngine::Label* scaleLabel = FindRowLabelFor(&root, fields[2]);
    ASSERT_NE(scaleLabel, nullptr);

    std::optional<GameEngine::InterceptableContextMenu::Capture> captured;
    InterceptorGuard guard(
        [&captured](GameEngine::InterceptableContextMenu::Capture&& capture)
        {
            captured = std::move(capture);
            return true;
        });

    // Control first: a left-click on the label must not open a menu.
    SendMouse(*scaleLabel, GameEngine::kEventMouseUp, 0);
    ASSERT_FALSE(captured.has_value()) << "a left-click opened the row context menu";

    SendMouse(*scaleLabel, GameEngine::kEventMouseUp, 1);
    ASSERT_TRUE(captured.has_value()) << "right-clicking the row label opened no menu";

    const GameEngine::InterceptableContextMenu::CapturedItem* resetItem = nullptr;
    bool sawCopy = false;
    bool sawPaste = false;
    for (const auto& item : captured->Items)
    {
        sawCopy = sawCopy || item.Path == "Copy Local Scale (X, Y, Z)";
        sawPaste = sawPaste || item.Path == "Paste Local Scale (X, Y, Z)";
        if (item.Path == "Reset Local Scale (X, Y, Z)")
            resetItem = &item;
    }
    EXPECT_TRUE(sawCopy) << "the row menu lost its Copy entry";
    EXPECT_TRUE(sawPaste) << "the row menu lost its Paste entry";
    ASSERT_NE(resetItem, nullptr) << "the row menu offers no Reset entry";
    EXPECT_TRUE(resetItem->Enabled) << "Reset is greyed out; it never depends on the clipboard";

    ASSERT_TRUE(static_cast<bool>(captured->Invoke));
    captured->Invoke(resetItem->CommandId);

    const Vector3 scale = world->GetComponent<Transform>(handle)->GetScale();
    EXPECT_NEAR(scale.x, 1.0f, 1.0e-5f);
    EXPECT_NEAR(scale.y, 1.0f, 1.0e-5f);
    EXPECT_NEAR(scale.z, 1.0f, 1.0e-5f);

    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Reset Local Scale");
    undo.Undo();
    EXPECT_NEAR(world->GetComponent<Transform>(handle)->GetScale().x, 2.0f, 1.0e-5f);
}

// The inspector's undo snapshot covers the whole selection, not just the primary:
// undoing a typed edit has to put every entity the edit moved back where it was.
TEST_F(UndoRedoServiceTests, TransformInspectorEditUndoRestoresEveryEntityInSelection)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::RegisterTransformInspector();

    const Quaternion rot(1.0f, 0.0f, 0.0f, 0.0f);
    const Vector3 scale(1.0f, 1.0f, 1.0f);
    Entity primary = world->Create();
    Entity peer = world->Create();
    primary.Set(Transform::FromTRS(Vector3(0.0f, 0.0f, 0.0f), rot, scale));
    peer.Set(Transform::FromTRS(Vector3(5.0f, 0.0f, 0.0f), rot, scale));
    world->ProcessCommands();

    const EntityHandle primaryHandle = primary.GetHandle();
    const EntityHandle peerHandle = peer.GetHandle();
    auto* primaryTransform = world->GetComponentForWrite<Transform>(primaryHandle);
    ASSERT_NE(primaryTransform, nullptr);

    UIElement root("TestRoot");
    UndoRedoService undo;

    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.World = world;
    ctx.Entity = primaryHandle;
    ctx.Entities = {primaryHandle, peerHandle};
    ctx.Object = primaryTransform;
    ctx.Undo = &undo;
    ctx.ChangeNotifications = nullptr;

    InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Transform>();
    ASSERT_NE(inspector, nullptr);
    (*inspector)(ctx);

    std::vector<Vector3Field*> fields = CollectVector3Fields(&root);
    ASSERT_GE(fields.size(), 1u);

    FloatField* xField = nullptr;
    for (const auto& ch : fields[0]->GetChildren())
    {
        if (auto* f = dynamic_cast<FloatField*>(ch.get()))
        {
            xField = f;
            break;
        }
    }
    ASSERT_NE(xField, nullptr);

    TextInput* editor = nullptr;
    for (const auto& ch : xField->GetChildren())
    {
        if (auto* ti = dynamic_cast<TextInput*>(ch.get()))
        {
            editor = ti;
            break;
        }
    }
    ASSERT_NE(editor, nullptr);

    editor->OnFocusChanged(true);
    editor->OnKey(GameEngine::Input::kKeyCode_A, 0x0002 /*GLFW_MOD_CONTROL*/, nullptr);
    editor->OnChar(static_cast<unsigned int>('2'));
    editor->OnChar(static_cast<unsigned int>('0'));
    editor->OnKey(257 /*GLFW_KEY_ENTER*/, 0, nullptr);

    ASSERT_EQ(undo.GetUndoCount(), 1u);

    // A value edit carries the peer by the primary's delta: 5 + 20.
    EXPECT_NEAR(world->GetComponent<Transform>(primaryHandle)->GetPosition().x, 20.0f, 1.0e-5f);
    EXPECT_NEAR(world->GetComponent<Transform>(peerHandle)->GetPosition().x, 25.0f, 1.0e-5f);

    undo.Undo();
    EXPECT_NEAR(world->GetComponent<Transform>(primaryHandle)->GetPosition().x, 0.0f, 1.0e-5f);
    EXPECT_NEAR(world->GetComponent<Transform>(peerHandle)->GetPosition().x, 5.0f, 1.0e-5f);

    undo.Redo();
    EXPECT_NEAR(world->GetComponent<Transform>(primaryHandle)->GetPosition().x, 20.0f, 1.0e-5f);
    EXPECT_NEAR(world->GetComponent<Transform>(peerHandle)->GetPosition().x, 25.0f, 1.0e-5f);
}

TEST_F(UndoRedoServiceTests, AnimatorSlidersPreviewLiveAndRemainUndoable)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::RegisterAnimatorInspector();

    Entity entity = world->Create();
    GameEngine::Components::Animator initial{};
    initial.speedScale = 1.0f;
    initial.blendSeconds = 0.25f;
    entity.Set(initial);
    world->ProcessCommands();

    const EntityHandle handle = entity.GetHandle();
    UIElement root("TestRoot");
    UndoRedoService undo;

    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.World = world;
    World* currentWorld = world;
    ctx.GetWorld = [&currentWorld]() { return currentWorld; };
    ctx.Entity = handle;
    ctx.Object = world->GetComponentForWrite<GameEngine::Components::Animator>(handle);
    ctx.Undo = &undo;
    ctx.ChangeNotifications = nullptr;

    InspectorFn* inspector =
        InspectorRegistry::Get().TryGetComponentInspector<GameEngine::Components::Animator>();
    ASSERT_NE(inspector, nullptr);
    (*inspector)(ctx);

    Slider* speedScaleSlider = nullptr;
    Slider* blendTimeSlider = nullptr;
    auto findAnimatorSliders = [&](UIElement* node, const auto& self) -> void
    {
        if (!node)
            return;
        if (auto* slider = dynamic_cast<Slider*>(node))
        {
            if (slider->GetMin() == 0.0f && slider->GetMax() == 3.0f)
                speedScaleSlider = slider;
            else if (slider->GetMin() == 0.0f && slider->GetMax() == 5.0f)
                blendTimeSlider = slider;
        }
        for (const auto& child : node->GetChildren())
            self(child.get(), self);
    };
    findAnimatorSliders(&root, findAnimatorSliders);

    ASSERT_NE(speedScaleSlider, nullptr);
    ASSERT_NE(blendTimeSlider, nullptr);
    EXPECT_FLOAT_EQ(speedScaleSlider->GetTrackPaddingPx(), 7.0f);
    EXPECT_FLOAT_EQ(blendTimeSlider->GetTrackPaddingPx(), 7.0f);

    auto exerciseSlider = [&](Slider* slider, float GameEngine::Components::Animator::* field,
                              float before, float after, const char* undoName)
    {
        slider->SetValueWithoutNotify(after);
        slider->NotifyValueChanging();

        auto* previewed = world->GetComponent<GameEngine::Components::Animator>(handle);
        ASSERT_NE(previewed, nullptr);
        EXPECT_FLOAT_EQ(previewed->*field, after);
        EXPECT_EQ(undo.GetUndoCount(), 0u);

        slider->NotifyValueChanged();
        EXPECT_EQ(undo.GetUndoCount(), 1u);
        EXPECT_STREQ(undo.PeekUndoName(), undoName);

        undo.Undo();
        auto* undone = world->GetComponent<GameEngine::Components::Animator>(handle);
        ASSERT_NE(undone, nullptr);
        EXPECT_FLOAT_EQ(undone->*field, before);

        undo.Redo();
        auto* redone = world->GetComponent<GameEngine::Components::Animator>(handle);
        ASSERT_NE(redone, nullptr);
        EXPECT_FLOAT_EQ(redone->*field, after);

        undo.Clear();
    };

    exerciseSlider(speedScaleSlider, &GameEngine::Components::Animator::speedScale,
                   initial.speedScale, 2.0f, "Change Animator Speed Scale");
    exerciseSlider(blendTimeSlider, &GameEngine::Components::Animator::blendSeconds,
                   initial.blendSeconds, 1.5f, "Change Animator Blend Time");

    // A committed snapshot remains attached to the world that was edited even
    // after the Inspector binds a replacement world.
    speedScaleSlider->SetValueWithoutNotify(2.5f);
    speedScaleSlider->NotifyValueChanging();
    speedScaleSlider->NotifyValueChanged();
    ASSERT_EQ(undo.GetUndoCount(), 1u);

    World reboundWorld;
    currentWorld = &reboundWorld;
    undo.Undo();
    const auto* undoneAfterRebind =
        world->GetComponent<GameEngine::Components::Animator>(handle);
    ASSERT_NE(undoneAfterRebind, nullptr);
    EXPECT_FLOAT_EQ(undoneAfterRebind->speedScale, 2.0f);
    undo.Clear();

    // If the rebind happens during a drag, the stale commit cancels the active
    // edit and restores its preview in the original world.
    currentWorld = world;
    speedScaleSlider->SetValueWithoutNotify(0.5f);
    speedScaleSlider->NotifyValueChanging();
    const auto* previewBeforeRebind =
        world->GetComponent<GameEngine::Components::Animator>(handle);
    ASSERT_NE(previewBeforeRebind, nullptr);
    EXPECT_FLOAT_EQ(previewBeforeRebind->speedScale, 0.5f);

    currentWorld = &reboundWorld;
    speedScaleSlider->NotifyValueChanged();

    const auto* originalWorldAnimator = world->GetComponent<GameEngine::Components::Animator>(handle);
    ASSERT_NE(originalWorldAnimator, nullptr);
    EXPECT_FLOAT_EQ(originalWorldAnimator->speedScale, 2.0f);
    EXPECT_EQ(undo.GetUndoCount(), 0u);
}

TEST_F(UndoRedoServiceTests, AnimatorInspectorHidesUnevaluatedPlaybackRows)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::RegisterAnimatorInspector();

    Entity entity = world->Create();
    entity.Set(GameEngine::Components::Animator{});
    world->ProcessCommands();

    const EntityHandle handle = entity.GetHandle();
    UIElement root("TestRoot");
    UndoRedoService undo;

    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.World = world;
    World* currentWorld = world;
    ctx.GetWorld = [&currentWorld]() { return currentWorld; };
    ctx.Entity = handle;
    ctx.Object = world->GetComponentForWrite<GameEngine::Components::Animator>(handle);
    ctx.Undo = &undo;
    ctx.ChangeNotifications = nullptr;

    InspectorFn* inspector =
        InspectorRegistry::Get().TryGetComponentInspector<GameEngine::Components::Animator>();
    ASSERT_NE(inspector, nullptr);
    (*inspector)(ctx);

    std::vector<std::string> labels;
    std::string controllerTooltip;
    auto collect = [&](UIElement* node, const auto& self) -> void
    {
        if (!node)
            return;
        if (auto* label = dynamic_cast<GameEngine::Label*>(node))
        {
            labels.push_back(label->GetText());
            if (label->GetText() == "Controller")
                controllerTooltip = label->GetTooltip();
        }
        for (const auto& child : node->GetChildren())
            self(child.get(), self);
    };
    collect(&root, collect);

    auto hasLabel = [&](const char* text)
    {
        return std::find(labels.begin(), labels.end(), text) != labels.end();
    };

    EXPECT_TRUE(hasLabel("Clip"));
    EXPECT_TRUE(hasLabel("Timeline"));
    EXPECT_TRUE(hasLabel("Controller"));
    EXPECT_TRUE(hasLabel("Loop"));
    EXPECT_TRUE(hasLabel("Active"));
    EXPECT_TRUE(hasLabel("Speed Scale"));
    EXPECT_TRUE(hasLabel("Blend Time"));
    EXPECT_TRUE(hasLabel("Root Node"));
    EXPECT_TRUE(hasLabel("Max Polyphony"));
    EXPECT_TRUE(hasLabel("Preview"));
    EXPECT_TRUE(hasLabel("Auto-play in play mode"));

    EXPECT_FALSE(hasLabel("Current Animation"));
    EXPECT_FALSE(hasLabel("Queued Animation"));
    EXPECT_FALSE(hasLabel("Source"));
    EXPECT_FALSE(hasLabel("Library"));
    EXPECT_FALSE(hasLabel("Library Animation"));
    EXPECT_FALSE(hasLabel("AnimationPlayer"));
    EXPECT_FALSE(hasLabel("AnimationMixer"));
    EXPECT_FALSE(hasLabel("Deterministic"));
    EXPECT_FALSE(hasLabel("Reset on Save"));
    EXPECT_FALSE(hasLabel("Root Motion"));
    EXPECT_FALSE(hasLabel("Apply Root Motion"));
    EXPECT_FALSE(hasLabel("Callback Mode"));

    EXPECT_NE(controllerTooltip.find("not evaluated"), std::string::npos);
}

TEST_F(UndoRedoServiceTests, BoundedComponentSliderCommitsOneUndoableDrag)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    Entity entity = world->Create();
    GameEngine::Components::Animator initial{};
    initial.speedScale = 1.0f;
    entity.Set(initial);
    world->ProcessCommands();

    const EntityHandle handle = entity.GetHandle();
    UIElement root("TestRoot");
    UndoRedoService undo;
    Slider* slider = GameEngine::InspectorDrag::AddComponentFloatSliderRow<
        GameEngine::Components::Animator>(
            &root, "Speed Scale", initial.speedScale, 0.0f, 3.0f,
            world, handle, nullptr, &undo, "Change Animator Speed Scale",
            [](GameEngine::Components::Animator& animator, float value)
            {
                animator.speedScale = value;
            });
    ASSERT_NE(slider, nullptr);

    for (float previewValue : {1.25f, 1.5f, 2.0f})
    {
        slider->SetValueWithoutNotify(previewValue);
        slider->NotifyValueChanging();
        ASSERT_NE(world->GetComponent<GameEngine::Components::Animator>(handle), nullptr);
        EXPECT_FLOAT_EQ(
            world->GetComponent<GameEngine::Components::Animator>(handle)->speedScale,
            previewValue);
        EXPECT_EQ(undo.GetUndoCount(), 0u);
    }
    ASSERT_NE(world->GetComponent<GameEngine::Components::Animator>(handle), nullptr);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(handle)->speedScale, 2.0f);

    slider->NotifyValueChanged();
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_STREQ(undo.PeekUndoName(), "Change Animator Speed Scale");

    undo.Undo();
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(handle)->speedScale, 1.0f);
    undo.Redo();
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(handle)->speedScale, 2.0f);
}

TEST_F(UndoRedoServiceTests, ComponentSliderDirectCommitGroupsMultiSelectionUndo)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::Components::Animator initial{};
    initial.speedScale = 1.0f;
    Entity primary = world->Create();
    Entity secondary = world->Create();
    primary.Set(initial);
    secondary.Set(initial);
    world->ProcessCommands();

    UndoRedoService undo;
    auto handlers = GameEngine::InspectorDrag::MakeComponentInteractiveHandlers<
        GameEngine::Components::Animator, float>(
            world,
            primary.GetHandle(),
            nullptr,
            &undo,
            "Change Animator Speed Scale",
            [](GameEngine::Components::Animator& animator, float value)
            {
                animator.speedScale = value;
            },
            {secondary.GetHandle()});

    // Typed values and double-click resets can commit without a preview event.
    handlers.second(2.0f);
    ASSERT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(primary.GetHandle())->speedScale, 2.0f);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(secondary.GetHandle())->speedScale, 2.0f);

    undo.Undo();
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(primary.GetHandle())->speedScale, 1.0f);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(secondary.GetHandle())->speedScale, 1.0f);
    undo.Redo();
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(primary.GetHandle())->speedScale, 2.0f);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(secondary.GetHandle())->speedScale, 2.0f);
}

TEST_F(UndoRedoServiceTests, ComponentSliderIgnoresPeersWithoutComponentForUndo)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::Components::Animator initial{};
    initial.speedScale = 1.0f;
    Entity primary = world->Create();
    Entity editablePeer = world->Create();
    Entity peerWithoutAnimator = world->Create();
    primary.Set(initial);
    editablePeer.Set(initial);
    world->ProcessCommands();

    UndoRedoService undo;
    auto handlers = GameEngine::InspectorDrag::MakeComponentInteractiveHandlers<
        GameEngine::Components::Animator, float>(
            world,
            primary.GetHandle(),
            nullptr,
            &undo,
            "Change Animator Speed Scale",
            [](GameEngine::Components::Animator& animator, float value)
            {
                animator.speedScale = value;
            },
            {editablePeer.GetHandle(), peerWithoutAnimator.GetHandle()});

    handlers.first(2.0f);
    handlers.second(2.0f);

    ASSERT_EQ(undo.GetUndoCount(), 1u);
    ASSERT_NE(world->GetComponent<GameEngine::Components::Animator>(primary.GetHandle()), nullptr);
    ASSERT_NE(world->GetComponent<GameEngine::Components::Animator>(editablePeer.GetHandle()), nullptr);
    EXPECT_EQ(world->GetComponent<GameEngine::Components::Animator>(peerWithoutAnimator.GetHandle()), nullptr);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(primary.GetHandle())->speedScale, 2.0f);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(editablePeer.GetHandle())->speedScale, 2.0f);

    undo.Undo();
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(primary.GetHandle())->speedScale, 1.0f);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(editablePeer.GetHandle())->speedScale, 1.0f);
    EXPECT_EQ(world->GetComponent<GameEngine::Components::Animator>(peerWithoutAnimator.GetHandle()), nullptr);

    undo.Redo();
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(primary.GetHandle())->speedScale, 2.0f);
    EXPECT_FLOAT_EQ(world->GetComponent<GameEngine::Components::Animator>(editablePeer.GetHandle())->speedScale, 2.0f);
    EXPECT_EQ(world->GetComponent<GameEngine::Components::Animator>(peerWithoutAnimator.GetHandle()), nullptr);
}

TEST_F(UndoRedoServiceTests, ComponentSliderDispatchesEachChangeNotificationOnce)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    GameEngine::Components::Animator initial{};
    initial.speedScale = 1.0f;
    Entity entity = world->Create();
    entity.Set(initial);
    world->ProcessCommands();

    const EntityHandle handle = entity.GetHandle();
    UndoRedoService undo;
    GameEngine::Editor::EditorChangeNotifications notifications;
    int previewCount = 0;
    int commitCount = 0;
    int undoRedoCount = 0;
    const auto subscription = notifications.SubscribeComponentChanged(
        [&](const GameEngine::Editor::EditorChangeNotifications::ComponentChangedEvent& event)
        {
            if (event.world != world || event.entity != handle)
                return;
            switch (event.kind)
            {
                case GameEngine::Editor::EditorChangeNotifications::ChangeKind::Preview:
                    ++previewCount;
                    break;
                case GameEngine::Editor::EditorChangeNotifications::ChangeKind::Commit:
                    ++commitCount;
                    break;
                case GameEngine::Editor::EditorChangeNotifications::ChangeKind::UndoRedo:
                    ++undoRedoCount;
                    break;
                case GameEngine::Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild:
                    break;
            }
        });
    ASSERT_TRUE(subscription);

    auto handlers = GameEngine::InspectorDrag::MakeComponentInteractiveHandlers<
        GameEngine::Components::Animator, float>(
            world,
            handle,
            &notifications,
            &undo,
            "Change Animator Speed Scale",
            [](GameEngine::Components::Animator& animator, float value)
            {
                animator.speedScale = value;
            },
            {});

    handlers.first(2.0f);
    EXPECT_EQ(previewCount, 1);
    EXPECT_EQ(commitCount, 0);

    handlers.second(2.0f);
    EXPECT_EQ(previewCount, 1);
    EXPECT_EQ(commitCount, 1);
    ASSERT_EQ(undo.GetUndoCount(), 1u);

    undo.Undo();
    EXPECT_EQ(undoRedoCount, 1);
    undo.Redo();
    EXPECT_EQ(undoRedoCount, 2);

    // Typed values and double-click resets commit without a preview event.
    handlers.second(1.5f);
    EXPECT_EQ(previewCount, 1);
    EXPECT_EQ(commitCount, 2);
}

TEST_F(UndoRedoServiceTests, CreateEntityUndoRedo_PreservesHandle_ForSubsequentTransformRedo)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    world->Clear();

    // Create an entity and apply initial Transform immediately (mirrors editor create path).
    EntityHandle h = world->CreateEntity();
    ASSERT_TRUE(h.IsValid());

    Vector3 startPos(1.0f, 2.0f, 3.0f);
    Quaternion rot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 scale(1.0f, 1.0f, 1.0f);
    Transform start = Transform::FromTRS(startPos, rot, scale);
    world->AddComponentImmediate(h, start);

    // Undo service with two commands: create + transform edit.
    UndoRedoService svc;

    // Create command that destroys/revives the same handle.
    class CreateCmd final : public GameEngine::Editor::IEditorCommand
    {
    public:
        CreateCmd(World* w, EntityHandle e, Transform t) : m_World(w), m_Entity(e), m_Transform(t) {}
        const char* GetName() const override { return "Create Empty Entity"; }
        void Do() override { Redo(); }
        void Undo() override
        {
            if (m_World) m_World->DestroyEntityImmediatePreserveHandle(m_Entity);
        }
        void Redo() override
        {
            if (!m_World) return;
            if (!m_World->ReviveEntityImmediatePreserveHandle(m_Entity)) return;
            m_World->AddComponentImmediate(m_Entity, m_Transform);
        }
    private:
        World* m_World = nullptr;
        EntityHandle m_Entity{};
        Transform m_Transform{};
    };

    svc.CommitAlreadyApplied(std::make_unique<CreateCmd>(world, h, start));

    // Transform snapshot edit on the created entity.
    const auto typeId = GameEngine::ECS::GetComponentTypeId<Transform>();
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = "Transform";
    target.Capture = [world, h, typeId](UndoRedoService::SnapshotTarget::Snapshot& out)
    {
        return world->CaptureComponentBytes(h, typeId, out);
    };
    target.Apply = [world, h, typeId](const UndoRedoService::SnapshotTarget::Snapshot& snap)
    {
        return world->ApplyComponentBytesImmediate(h, typeId, snap);
    };

    Vector3 endPos(4.0f, -1.0f, 0.25f);
    Transform end = Transform::FromTRS(endPos, rot, scale);

    auto edit = svc.BeginInteractiveEdit("Transform Translate", target);
    ASSERT_TRUE((bool)edit);
    edit.Preview([&]()
                 {
                     world->AddComponentImmediate(h, end);
                 });
    edit.Commit();

    EXPECT_TRUE(svc.CanUndo());
    EXPECT_EQ(svc.GetUndoCount(), 2u);

    // Undo transform
    svc.Undo();
    EXPECT_TRUE(world->IsValid(h));
    {
        auto* t = world->GetComponent<Transform>(h);
        ASSERT_NE(t, nullptr);
        Vector3 p =t->GetPosition();
        EXPECT_NEAR(p.x, startPos.x, 1.0e-5f);
        EXPECT_NEAR(p.y, startPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, startPos.z, 1.0e-5f);
    }

    // Undo create -> entity should no longer be valid
    svc.Undo();
    EXPECT_FALSE(world->IsValid(h));

    // Redo create -> entity should be revived with initial transform
    svc.Redo();
    EXPECT_TRUE(world->IsValid(h));
    {
        auto* t = world->GetComponent<Transform>(h);
        ASSERT_NE(t, nullptr);
        Vector3 p =t->GetPosition();
        EXPECT_NEAR(p.x, startPos.x, 1.0e-5f);
        EXPECT_NEAR(p.y, startPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, startPos.z, 1.0e-5f);
    }

    // Redo transform -> should still apply to the same handle after revive
    svc.Redo();
    EXPECT_TRUE(world->IsValid(h));
    {
        auto* t = world->GetComponent<Transform>(h);
        ASSERT_NE(t, nullptr);
        Vector3 p =t->GetPosition();
        EXPECT_NEAR(p.x, endPos.x, 1.0e-5f);
        EXPECT_NEAR(p.y, endPos.y, 1.0e-5f);
        EXPECT_NEAR(p.z, endPos.z, 1.0e-5f);
    }
}

TEST_F(UndoRedoServiceTests, CreateAsEmptyParent_PreservesChildWorldMatrix_UndoRedo)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    world->Clear();

    // Create a parent with a non-trivial local transform.
    EntityHandle parent = world->CreateEntity();
    ASSERT_TRUE(parent.IsValid());
    Transform parentT = Transform::FromTRS(Vector3(3.0f, 1.0f, -2.0f), Quaternion{}, Vector3(2.0f, 1.0f, 0.5f));
    world->AddComponentImmediate(parent, parentT);

    // Create a child with a non-trivial local transform under the parent.
    EntityHandle child = world->CreateEntity();
    ASSERT_TRUE(child.IsValid());
    Transform childLocal = Transform::FromTRS(Vector3(-1.0f, 0.5f, 2.0f), Quaternion{}, Vector3(1.0f, 2.0f, 1.0f));
    world->AddComponentImmediate(child, childLocal);
    world->AddComponentImmediate(child, Parent{parent});

    const std::array<float, 16> worldBefore = ComputeWorldMatrix(world, child);

    UndoRedoService svc;

    // Apply the same algorithm as HierarchyPanel's "Create/As Empty Parent" and commit to undo stack.
    EntityHandle newParent = world->CreateEntity();
    ASSERT_TRUE(newParent.IsValid());

    // Capture originals for undo
    Transform childTransformBefore = childLocal;
    Parent childParentBefore{parent};
    bool childHadParent = true;

    // Do: insert parent between old parent and child; newParent takes child's local; child becomes identity.
    world->AddComponentImmediate(newParent, childTransformBefore);
    world->AddComponentImmediate(newParent, childParentBefore);
    world->AddComponentImmediate(child, Parent{newParent});
    Transform identity{};
    identity.SetIdentity();
    world->AddComponentImmediate(child, identity);

    class InsertParentCmd final : public GameEngine::Editor::IEditorCommand
    {
    public:
        InsertParentCmd(World* w, EntityHandle c, EntityHandle np, bool hadParent, Parent beforeParent, Transform beforeXf)
            : m_World(w), m_Child(c), m_NewParent(np), m_HadParent(hadParent), m_BeforeParent(beforeParent), m_BeforeXf(beforeXf) {}
        const char* GetName() const override { return "Create Empty Parent"; }
        void Do() override { Redo(); }
        void Undo() override
        {
            if (!m_World) return;
            // Restore child first.
            m_World->AddComponentImmediate(m_Child, m_BeforeXf);
            if (m_HadParent) m_World->AddComponentImmediate(m_Child, m_BeforeParent);
            else m_World->RemoveComponentImmediate<Parent>(m_Child);
            m_World->DestroyEntityImmediatePreserveHandle(m_NewParent);
        }
        void Redo() override
        {
            if (!m_World) return;
            if (!m_World->ReviveEntityImmediatePreserveHandle(m_NewParent)) return;
            m_World->AddComponentImmediate(m_NewParent, m_BeforeXf);
            if (m_HadParent) m_World->AddComponentImmediate(m_NewParent, m_BeforeParent);
            else m_World->RemoveComponentImmediate<Parent>(m_NewParent);
            m_World->AddComponentImmediate(m_Child, Parent{m_NewParent});
            Transform id{}; id.SetIdentity();
            m_World->AddComponentImmediate(m_Child, id);
        }
    private:
        World* m_World = nullptr;
        EntityHandle m_Child{};
        EntityHandle m_NewParent{};
        bool m_HadParent = false;
        Parent m_BeforeParent{};
        Transform m_BeforeXf{};
    };

    svc.CommitAlreadyApplied(std::make_unique<InsertParentCmd>(world, child, newParent, childHadParent, childParentBefore, childTransformBefore));

    // After insertion, child's world matrix should be unchanged.
    const std::array<float, 16> worldAfter = ComputeWorldMatrix(world, child);
    ExpectMat4Near(worldAfter, worldBefore);

    // Undo insertion: world matrix should still match the original.
    svc.Undo();
    EXPECT_TRUE(world->IsValid(child));
    const std::array<float, 16> worldUndo = ComputeWorldMatrix(world, child);
    ExpectMat4Near(worldUndo, worldBefore);

    // Redo insertion: world matrix should still match the original.
    svc.Redo();
    EXPECT_TRUE(world->IsValid(child));
    const std::array<float, 16> worldRedo = ComputeWorldMatrix(world, child);
    ExpectMat4Near(worldRedo, worldBefore);
}

class NoOpCommand final : public GameEngine::Editor::IEditorCommand
{
public:
    const char* GetName() const override { return "NoOp"; }
    void Do() override {}
    void Undo() override {}
};

// Play mode detaches the edit history on enter and re-attaches it on exit; the
// History panel and get_undo_stack read the timestamps after that round trip.
TEST_F(UndoRedoServiceTests, DetachAttachHistoryPreservesUndoTimestamps)
{
    UndoRedoService svc;
    svc.Execute(std::make_unique<NoOpCommand>());
    svc.Execute(std::make_unique<NoOpCommand>());
    ASSERT_EQ(svc.GetUndoCount(), 2u);

    const auto first = svc.GetUndoTimestampAt(0);
    const auto second = svc.GetUndoTimestampAt(1);
    ASSERT_NE(first, std::chrono::system_clock::time_point{});
    ASSERT_NE(second, std::chrono::system_clock::time_point{});

    UndoRedoService::History history = svc.DetachHistory();
    EXPECT_EQ(svc.GetUndoCount(), 0u);
    svc.AttachHistory(std::move(history));

    ASSERT_EQ(svc.GetUndoCount(), 2u);
    EXPECT_EQ(svc.GetUndoTimestampAt(0), first);
    EXPECT_EQ(svc.GetUndoTimestampAt(1), second);
}

// A step's id travels with it: undone, redone, and through play mode's detach, on either
// stack; the AI Assistant's ledger names its steps by these ids.
TEST_F(UndoRedoServiceTests, AnEntryKeepsItsIdThroughUndoRedoAndDetach)
{
    UndoRedoService svc;
    svc.Execute(std::make_unique<NoOpCommand>());
    svc.Execute(std::make_unique<NoOpCommand>());
    const std::uint64_t first = svc.GetUndoEntryIdAt(0);
    const std::uint64_t second = svc.GetUndoEntryIdAt(1);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);
    ASSERT_NE(first, second);

    svc.Undo();
    EXPECT_EQ(svc.GetRedoEntryIdAt(0), second);
    svc.Redo();
    EXPECT_EQ(svc.GetUndoEntryIdAt(1), second);

    svc.Undo();
    svc.AttachHistory(svc.DetachHistory());
    EXPECT_EQ(svc.GetUndoEntryIdAt(0), first);
    EXPECT_EQ(svc.GetRedoEntryIdAt(0), second);
    svc.Redo();
    EXPECT_EQ(svc.GetUndoEntryIdAt(1), second);

    svc.Undo();
    svc.Execute(std::make_unique<NoOpCommand>());
    EXPECT_NE(svc.GetUndoEntryIdAt(1), second) << "an id is never reused";
}

TEST_F(UndoRedoServiceTests, TrimmingTheHistoryDropsTheOldestIds)
{
    UndoRedoService svc(UndoRedoService::Config{2});
    svc.Execute(std::make_unique<NoOpCommand>());
    svc.Execute(std::make_unique<NoOpCommand>());
    const std::uint64_t second = svc.GetUndoEntryIdAt(1);
    svc.Execute(std::make_unique<NoOpCommand>());

    ASSERT_EQ(svc.GetUndoCount(), 2u);
    EXPECT_EQ(svc.GetUndoEntryIdAt(0), second);
    EXPECT_GT(svc.GetUndoEntryIdAt(1), second);
}

TEST_F(UndoRedoServiceTests, AnInteractiveEditIsOpenUntilItCommitsCancelsOrIsDestroyed)
{
    UndoRedoService svc;
    int value = 0;
    UndoRedoService::SnapshotTarget target;
    target.Capture = [&value](UndoRedoService::SnapshotTarget::Snapshot& out) {
        out.assign(reinterpret_cast<const std::uint8_t*>(&value),
                   reinterpret_cast<const std::uint8_t*>(&value) + sizeof(value));
        return true;
    };
    target.Apply = [&value](const UndoRedoService::SnapshotTarget::Snapshot& in) {
        std::memcpy(&value, in.data(), sizeof(value));
        return true;
    };
    EXPECT_FALSE(svc.HasOpenInteractiveEdit());

    UndoRedoService::InteractiveEdit committed = svc.BeginInteractiveEdit("Drag", target);
    EXPECT_TRUE(svc.HasOpenInteractiveEdit());
    UndoRedoService::InteractiveEdit moved = std::move(committed);
    EXPECT_TRUE(svc.HasOpenInteractiveEdit());
    value = 1;
    moved.Commit();
    EXPECT_FALSE(svc.HasOpenInteractiveEdit());

    UndoRedoService::InteractiveEdit cancelled = svc.BeginInteractiveEdit("Drag", target);
    cancelled.Cancel();
    EXPECT_FALSE(svc.HasOpenInteractiveEdit());

    {
        UndoRedoService::InteractiveEdit dropped = svc.BeginInteractiveEdit("Drag", target);
        EXPECT_TRUE(svc.HasOpenInteractiveEdit());
    }
    EXPECT_FALSE(svc.HasOpenInteractiveEdit());
}

} // namespace
