// Action authoring through the scripting ABI, end to end on the runtime sink.
//
// The managed surface (Managed/Input.ABI/Input.cs) exposes RegisterAction, Bind
// and GetActionState and does NOT expose PushContext, so a script's context is
// only ever registered and enabled — never pushed. These drive the same C
// exports that façade binds, so the chain a script actually gets is what is
// pinned here rather than the C++ API underneath it.
#include "gtest/gtest.h"

#include "Core/Engine.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Scripting/ScriptingABI.h"

namespace
{
// Ids the way the façade produces them (Input.Id -> InputIds.HashInput).
constexpr uint64_t kScriptContext = GameEngine::Input::HashInput("Script.Gameplay");
constexpr uint64_t kMoveForward = GameEngine::Input::HashInput("Script.Gameplay.MoveForward");
constexpr int32_t kDeviceKeyboard = 0; // Input.DeviceType.Keyboard

// Points the ABI at a sink this test owns, and puts the engine back afterwards
// so no other suite inherits it.
struct ScopedRuntimeInput
{
    explicit ScopedRuntimeInput(GameEngine::Input::InputSystem& sink)
    {
        GameEngine::EngineCore::GetInstance().SetRuntimeInput(&sink);
    }
    ~ScopedRuntimeInput() { GameEngine::EngineCore::GetInstance().SetRuntimeInput(nullptr); }
};
} // namespace

TEST(ScriptingAbi_Input, BoundActionIsReadableWithoutPushingAContext)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);

    ASSERT_EQ(GE_Input_RegisterAction(kScriptContext, kMoveForward, /*isAxis=*/0), GE_Result_Ok);
    ASSERT_EQ(GE_Input_Bind(kScriptContext, kMoveForward, kDeviceKeyboard, GameEngine::Input::kKeyCode_W,
                            /*scale=*/1.0f, /*requiredMods=*/0, /*forbiddenMods=*/0),
              GE_Result_Ok);

    // The binding claims the key, so the chain stops here and nothing downstream
    // sees the press. The action it was claimed for must therefore be live.
    EXPECT_TRUE(sink.OnKey(GameEngine::Input::kKeyCode_W, GameEngine::Input::kKeyActionPress, /*mods=*/0));
    sink.Update(0.016f);

    GE_InputActionState state{};
    ASSERT_EQ(GE_Input_GetActionState(kMoveForward, &state), GE_Result_Ok);
    EXPECT_NE(state.pressed, 0u) << "a claimed key must not read as a dead action";
    EXPECT_NE(state.justPressed, 0u);

    sink.OnKey(GameEngine::Input::kKeyCode_W, GameEngine::Input::kKeyActionRelease, /*mods=*/0);
    sink.Update(0.016f);

    state = GE_InputActionState{};
    ASSERT_EQ(GE_Input_GetActionState(kMoveForward, &state), GE_Result_Ok);
    EXPECT_EQ(state.pressed, 0u);
    EXPECT_NE(state.justReleased, 0u);
}

TEST(ScriptingAbi_Input, DisablingTheContextWithdrawsTheClaimAndTheAction)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);

    ASSERT_EQ(GE_Input_RegisterAction(kScriptContext, kMoveForward, /*isAxis=*/0), GE_Result_Ok);
    ASSERT_EQ(GE_Input_Bind(kScriptContext, kMoveForward, kDeviceKeyboard, GameEngine::Input::kKeyCode_W,
                            /*scale=*/1.0f, /*requiredMods=*/0, /*forbiddenMods=*/0),
              GE_Result_Ok);
    ASSERT_EQ(GE_Input_SetContextEnabled(kScriptContext, /*enabled=*/0), GE_Result_Ok);

    // Both answers are judged on the same enabled contexts: a script that switches
    // its context off stops claiming the key and stops reading the action.
    EXPECT_FALSE(sink.OnKey(GameEngine::Input::kKeyCode_W, GameEngine::Input::kKeyActionPress, /*mods=*/0));
    sink.Update(0.016f);

    GE_InputActionState state{};
    ASSERT_EQ(GE_Input_GetActionState(kMoveForward, &state), GE_Result_Ok);
    EXPECT_EQ(state.pressed, 0u);
}

// The position is always readable — (0,0) before the pointer first reaches the
// window — and the two states say whether it is one to act on, so a script
// neither unwraps a missing value nor mistakes the origin for a real corner.
TEST(ScriptingAbi_Input, PointerStatesSayWhetherThePositionIsOneToActOn)
{
    GameEngine::Input::InputSystem sink{GameEngine::Input::SinkRole::Gameplay};
    ScopedRuntimeInput scoped(sink);

    float x = -7.0f;
    float y = -7.0f;
    int32_t inWindow = 42;
    int32_t focused = 42;
    ASSERT_EQ(GE_Input_GetMousePosition(&x, &y), GE_Result_Ok);
    EXPECT_EQ(x, 0.0f);
    EXPECT_EQ(y, 0.0f);
    ASSERT_EQ(GE_Input_IsPointerInWindow(&inWindow), GE_Result_Ok);
    EXPECT_EQ(inWindow, 0);
    ASSERT_EQ(GE_Input_IsWindowFocused(&focused), GE_Result_Ok);
    EXPECT_EQ(focused, 0);

    sink.OnMouseMove({12.0f, 34.0f});
    sink.OnWindowFocus(true);
    ASSERT_EQ(GE_Input_GetMousePosition(&x, &y), GE_Result_Ok);
    EXPECT_EQ(x, 12.0f);
    EXPECT_EQ(y, 34.0f);
    ASSERT_EQ(GE_Input_IsPointerInWindow(&inWindow), GE_Result_Ok);
    EXPECT_EQ(inWindow, 1);
    ASSERT_EQ(GE_Input_IsWindowFocused(&focused), GE_Result_Ok);
    EXPECT_EQ(focused, 1);

    // Leaving and losing focus clear the states; the last position stands.
    sink.OnMouseLeave();
    sink.OnWindowFocus(false);
    ASSERT_EQ(GE_Input_GetMousePosition(&x, &y), GE_Result_Ok);
    EXPECT_EQ(x, 12.0f);
    EXPECT_EQ(y, 34.0f);
    ASSERT_EQ(GE_Input_IsPointerInWindow(&inWindow), GE_Result_Ok);
    EXPECT_EQ(inWindow, 0);
    ASSERT_EQ(GE_Input_IsWindowFocused(&focused), GE_Result_Ok);
    EXPECT_EQ(focused, 0);

    EXPECT_EQ(GE_Input_IsPointerInWindow(nullptr), GE_Result_InvalidArg);
    EXPECT_EQ(GE_Input_IsWindowFocused(nullptr), GE_Result_InvalidArg);
}
