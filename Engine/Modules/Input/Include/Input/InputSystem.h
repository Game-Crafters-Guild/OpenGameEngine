#pragma once

#include "Input/GamepadCodes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "Mathematics/Vector2.h"
#include "Types/StringId.h"
#include <vector>

namespace GameEngine
{
namespace Input
{

// Pointer positions, pointer deltas and scroll deltas are Mathematics::Vector2
// throughout this module: what a sink is handed is what its accessors report.
//
// A position is in the pixel space the sink is fed from — window client pixels
// for the application sink, play-surface-local pixels for a gameplay sink —
// with the origin at the top-left corner and +Y pointing down, the same
// convention the UI module lays out in. Coordinates are float, not integer,
// because the platform delivers sub-pixel cursor positions.

using InputId = StringId;

// Simple constexpr FNV-1a 64-bit hash for input ids (actions, contexts).
constexpr InputId HashInput(std::string_view sv)
{
    return HashStringId(sv);
}

using ActionId = InputId;  // e.g. HashInput("Editor.SceneView.MoveForward")
using ContextId = InputId; // e.g. HashInput("Editor.SceneView")

// For now we reuse platform (GLFW) key/button codes as integers.
using KeyCode = int;
using MouseCode = int;

// Key/button action values. These match GLFW's callback action integers, but
// app/editor code should use the input-layer names instead of including GLFW.
constexpr int kKeyActionRelease = 0;
constexpr int kKeyActionPress = 1;
constexpr int kKeyActionRepeat = 2;

// Synthetic mouse axis codes for ActionBinding::code when device == Mouse and isAxis == true.
// We use negative values to avoid colliding with real button codes (which are >= 0).
constexpr int kMouseAxisX = -1;       // horizontal mouse delta (pixels per frame)
constexpr int kMouseAxisY = -2;       // vertical mouse delta (pixels per frame)
constexpr int kMouseScrollAxisX = -3; // horizontal scroll delta per frame
constexpr int kMouseScrollAxisY = -4; // vertical scroll delta per frame;

// Modifier mask bits. Values follow GLFW's modifier bit layout so they can be
// sourced directly from platform callbacks without including GLFW headers.
enum ModifierMask : int
{
    kModShift = 0x0001,
    kModControl = 0x0002,
    kModAlt = 0x0004,
    kModSuper = 0x0008,
    kModCapsLock = 0x0010,
    kModNumLock = 0x0020,
};

// Common mask for differentiating shortcut-style modifier combinations.
constexpr int kModShortcutMask = kModShift | kModControl | kModAlt | kModSuper;

enum class DeviceType
{
    Keyboard,
    Mouse,
    Gamepad,
    Unknown
};

struct ActionState
{
    bool pressed = false;      // currently held
    bool justPressed = false;  // transitioned to pressed this frame
    bool justReleased = false; // transitioned to released this frame
    float value = 0.0f;        // for axes (e.g. -1..1, scroll delta)
};

struct ActionBinding
{
    DeviceType device = DeviceType::Keyboard;
    int code = 0;       // key/button/axis code (platform-specific for now)
    float scale = 1.0f; // for analog axes

    // Optional modifier requirements. If set, the binding is only considered when:
    // - (currentMods & requiredMods) == requiredMods
    // - (currentMods & forbiddenMods) == 0
    //
    // This allows distinguishing e.g. A from Shift+A by setting forbiddenMods on
    // the plain binding.
    int requiredMods = 0;
    int forbiddenMods = 0;
};

struct ActionDesc
{
    ActionId id;
    std::vector<ActionBinding> bindings;
    bool isAxis = false;
    // The host's own shortcut: while a game runs in the host's window, a key
    // bound to this action goes to the host ahead of the game
    // (WindowInputRouter::RouteKey). Survives rebinding the action's keys.
    bool hostReserved = false;
};

struct ActionEvent
{
    ActionId id;
    ActionState state;
    bool consumed = false; // when true, stop propagation to lower-priority contexts
};

using ActionCallback = std::function<void(ActionEvent&)>;

// Where a sink sits in a window's input chain.
//
// A Gameplay sink runs ahead of the rest of the chain (editor focus navigation
// and editor actions), so it takes exactly the presses it CLAIMS: a press
// matching an action binding of an enabled context, or a code some caller is
// polling. Delivery is consumption for it — it records an edge only for a press
// it took — so its edge queries and the routing chain can never tell different
// stories about who acted on a key.
//
// An Application sink ends the chain. Nothing downstream can receive what it
// declines, so it takes every press it is delivered.
enum class SinkRole
{
    Application,
    Gameplay,
};

// Sink for raw input events from the platform layer.
//
// The press-like entries answer whether this sink consumed the event; the router
// stops a consumed press there and keeps propagating everything else. Releases,
// moves, leaves and focus changes reach every stage unconditionally; the ones
// that answer, answer false.
class IRawInputSink
{
  public:
    virtual ~IRawInputSink() = default;

    /// A key press, repeat or release from the platform.
    /// @param key    Engine key code (KeyCodes.h).
    /// @param action kKeyActionPress, kKeyActionRepeat or kKeyActionRelease.
    /// @param mods   Platform modifier mask (kModShortcutMask bits) as held when
    ///               the event was generated.
    /// @return Whether this sink consumed a press; releases always answer false.
    virtual bool OnKey(int key, int action, int mods) = 0;
    virtual void OnChar(unsigned int codepoint) = 0;
    /// A mouse button transition. Pointer entries carry the platform's modifier
    /// mask for the same reason keys do: a chorded binding cannot be judged
    /// without it, and a sink's own key state is not a substitute — it never
    /// learns about a modifier it declined.
    /// @param button Mouse button code (KeyCodes.h).
    /// @param down   True for a press, false for a release.
    /// @param mods   Platform modifier mask (kModShortcutMask bits) as held when
    ///               the event was generated.
    /// @return Whether this sink consumed a press; releases always answer false.
    virtual bool OnMouseButton(int button, bool down, int mods) = 0;
    /// The pointer's new position, in this sink's pixel space. The first move
    /// after a leave is what puts the pointer back in the window.
    virtual void OnMouseMove(Mathematics::Vector2 position) = 0;
    /// The pointer left the window. Its last position stands; what is gone is
    /// its presence, which the next move restores.
    virtual void OnMouseLeave() = 0;
    /// The window gained or lost focus. Like a move, never consumed: focus is
    /// state every stage keeps its own view of.
    virtual void OnWindowFocus(bool focused) = 0;
    /// A wheel or trackpad scroll.
    /// @param delta Scroll delta in platform wheel units, horizontal in x.
    /// @param mods  Platform modifier mask (kModShortcutMask bits) as held when
    ///              the event was generated.
    /// @return Whether this sink consumed the scroll.
    virtual bool OnMouseScroll(Mathematics::Vector2 delta, int mods) = 0;

    /// Continuous state for one gamepad slot: whether a pad is there and where
    /// its axes stand. Never consumed — like pointer position it is state every
    /// stage keeps its own view of, so it reaches all of them.
    /// @param gamepadIndex Slot index (0 is the first pad).
    /// @param axes         Mapped axes in GamepadAxis order; null when the slot is empty.
    /// @param axisCount    Number of entries in `axes`.
    /// @param connected    Whether a pad occupies the slot.
    virtual void OnGamepadState(int gamepadIndex, const float* axes, int axisCount, bool connected) = 0;
    /// A gamepad button transition, differenced from the per-frame poll. A pad
    /// carries no modifiers, so there is no mask to judge a chord against.
    /// @param gamepadIndex Slot index (0 is the first pad).
    /// @param button       Gamepad button code (GamepadCodes.h).
    /// @param down         True for a press, false for a release.
    /// @return Whether this sink consumed a press; releases always answer false.
    virtual bool OnGamepadButton(int gamepadIndex, int button, bool down) = 0;
};

// High-level input system combining raw state, action mapping, and contexts.
class InputSystem : public IRawInputSink
{
  public:
    struct ListenerHandle
    {
        uint32_t id = 0;
    };

    explicit InputSystem(SinkRole role = SinkRole::Application);
    ~InputSystem();

    // Per-frame update (computes justPressed/justReleased, clears transient state)
    void Update(float deltaTime);

    // Release all keys/buttons and clear transient state, so no key appears
    // stuck down: OnWindowFocus(false) does this, and so does the editor when it
    // switches game input off. Leaves the pointer and focus states alone.
    void ResetState();

    // Clear all action registrations, context stack, and listeners. Called
    // when the runtime input session ends (e.g. play mode exit) to prevent
    // stale callbacks from accumulating across sessions.
    void ClearRegistrations();

    // A suspended Gameplay sink claims nothing: a paused game acts on no input,
    // so its keys belong to whatever is downstream (editor chrome, editor
    // actions) until it resumes. Releases still land and still clear the keys the
    // sink already holds, so a pause cannot strand one down. An Application sink
    // is unaffected — it has nothing downstream to hand its input to.
    void SetClaimsSuspended(bool suspended);

    // --- Raw polling layer ---
    // Polling is how interest is declared: every query for a consumable code
    // claims it for a short sliding window, refreshed by each poll. A Gameplay
    // sink therefore consumes the keys and buttons its game reads without the
    // game registering anything, and a query that returns false is still a poll.
    // The live position and axes carry no claim: they are state no stage can
    // consume. A per-button press origin is a button query and claims like one.
    bool IsKeyDown(KeyCode key) const;
    bool WasKeyPressed(KeyCode key) const;
    bool WasKeyReleased(KeyCode key) const;

    // Where the last move placed the pointer, in this sink's pixel space; (0,0)
    // before any move. It stands after the pointer leaves the window and after
    // the window loses focus, so a reader gates on the two states below rather
    // than on the position: the origin is where a pointer the platform never
    // placed would otherwise be read, and a game edge-scrolls toward it.
    Mathematics::Vector2 GetMousePosition() const;
    // True from the first move that reaches this sink until the pointer leaves
    // the window, and again from the next move. The platform keeps delivering
    // moves while a button is held, so a drag that crosses the window edge stays
    // in. Hover-style reads want this alone. A sink fed by several windows (the
    // editor's application input) answers for the last move or leave any of them
    // sent, and one window's leave can arrive after the next window's first move.
    bool IsPointerInWindow() const;
    // Whether the window feeding this sink holds focus, from the platform's
    // focus reports; false until one arrives. Gestures that act on the world —
    // edge scrolling, drags, wheel zoom — want this as well as the pointer in
    // the window: a window behind another still receives moves and the wheel
    // for a pointer that merely crosses it.
    bool IsWindowFocused() const;
    // Where the most recent accepted up-to-down transition happened, in the same
    // space as GetMousePosition(). Movement, a duplicate down, the release and
    // Update() all retain it; only another accepted press replaces it, and
    // ResetState() clears it. Empty before any press, for a press made while the
    // pointer is outside the window, and for an invalid button, so a gesture
    // that never began cannot be measured against a stale origin.
    // Like the other button queries, polling a valid button claims it even while
    // empty. This is one latched origin per button, not an event history.
    std::optional<Mathematics::Vector2> GetMouseButtonPressPosition(MouseCode button) const;
    bool IsMouseButtonDown(MouseCode button) const;
    // Frame-snapshot edge queries (mouse mirror of WasKeyPressed/WasKeyReleased).
    // Both report true for a press+release pair that completed inside a single
    // frame — the case an IsMouseButtonDown poll silently loses.
    bool WasMouseButtonPressed(MouseCode button) const;
    bool WasMouseButtonReleased(MouseCode button) const;

    // Gamepad state, in the standard layout GamepadCodes.h enumerates. Axes and
    // connection arrive as state, buttons as the edges the poll differences, so
    // a button reads down here exactly while this sink holds it.
    bool IsGamepadConnected(int gamepadIndex) const;
    float GetGamepadAxis(int gamepadIndex, GamepadAxis axis) const;
    bool IsGamepadButtonDown(int gamepadIndex, GamepadButton button) const;

    // --- Action registration & mapping ---
    void RegisterAction(const ContextId& context, const ActionDesc& desc);
    // Match the current keyboard bindings of one action in an enabled context.
    // This queries binding configuration only: no polling claims, frame-state
    // changes, or callbacks. Context stack priority does not disable bindings.
    bool MatchesKeyBinding(const ContextId& context, const ActionId& action, KeyCode key, int mods) const;
    // True when a hostReserved action of any enabled context is bound to this
    // key and modifier set. Same side-effect-free query as MatchesKeyBinding.
    bool IsHostReservedKey(KeyCode key, int mods) const;
    void RemoveAction(const ContextId& context, const ActionId& id);

    void BindKey(const ContextId& context, const ActionId& id, const ActionBinding& binding);
    void ClearBindings(const ContextId& context, const ActionId& id);

    // --- Context management ---
    void PushContext(const ContextId& context);
    void PopContext(const ContextId& context);
    void SetContextEnabled(const ContextId& context, bool enabled);

    // --- Action polling ---
    // Answers from the pushed contexts in priority order, then from any other
    // enabled context holding the action. Pushing is how priority is expressed,
    // not how an action is made readable: an action registered in an enabled
    // context is live, which is the same rule that decides whether its key is
    // claimed. Contexts that were never pushed carry no order among themselves.
    ActionState GetActionState(const ActionId& id) const;
    bool IsActionActive(const ActionId& id) const;
    bool WasActionTriggered(const ActionId& id) const;

    // --- Callbacks ---
    ListenerHandle AddActionListener(const ActionId& id, ActionCallback cb);
    void RemoveActionListener(ListenerHandle handle);

    // --- Persistence (sketch) ---
    bool SerializeBindingsToJson(std::string& outJson) const;
    bool DeserializeBindingsFromJson(const std::string& json);

    // IRawInputSink implementation (called from Platform::Window)
    bool OnKey(int key, int action, int mods) override;
    void OnChar(unsigned int codepoint) override;
    bool OnMouseButton(int button, bool down, int mods) override;
    void OnMouseMove(Mathematics::Vector2 position) override;
    void OnMouseLeave() override;
    void OnWindowFocus(bool focused) override;
    bool OnMouseScroll(Mathematics::Vector2 delta, int mods) override;
    void OnGamepadState(int gamepadIndex, const float* axes, int axisCount, bool connected) override;
    bool OnGamepadButton(int gamepadIndex, int button, bool down) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Input
} // namespace GameEngine
