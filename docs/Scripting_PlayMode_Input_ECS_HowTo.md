## How To: Use C# scripting with Play Mode + Input + ECS

This guide covers:
- **How Play Mode calls your C# code**
- **How to read input** (via `GameEngine.Input.ABI.dll`)
- **How to read/write ECS** (via `GameEngine.ECS.ABI.dll`)
- **How the Editor gates input** (only when **Game View is focused**)

### What you get today (V1)
- **Play Mode lifecycle hooks**: `[PlayModeEnter]`, `[PlayModeTick]`, `[PlayModeExit]` on **static methods**
- **Input ABI** (polling + user-defined actions/bindings), but **context gating is editor-owned**
- **ECS ABI**:
  - basic entity/name/transform helpers
  - **runtime-defined “blob” components** (C# `unmanaged` structs stored as bytes)
  - **type-erased query handle** (`CreateQuery` / `TryNext`)

---

## Play Mode lifecycle in C#

### Attributes
These live in `GameEngine.Scripting.ABI`:
- `GameEngine.Scripting.PlayModeEnterAttribute`
- `GameEngine.Scripting.PlayModeTickAttribute`
- `GameEngine.Scripting.PlayModeExitAttribute`

Supported signatures:
- `void OnEnter()` or `int OnEnter()`
- `void Tick(float deltaSeconds)` or `int Tick(float deltaSeconds)`
- `void OnExit()` or `int OnExit()`

### Minimal example

```csharp
using System;
using GameEngine.Scripting;

namespace GameScripts
{
    public static class MyGame
    {
        [PlayModeEnter]
        public static void OnEnter()
        {
            Console.WriteLine("[Game] Enter");
        }

        [PlayModeTick]
        public static void Tick(float dt)
        {
            // Called only while playing (not while paused).
        }

        [PlayModeExit]
        public static void OnExit()
        {
            Console.WriteLine("[Game] Exit");
        }
    }
}
```

---

## Input in C# (GameEngine.Input.ABI)

### Key rule: Editor-owned focus gating
The Editor enables gameplay input only when:
- **Play Mode is active**, and
- the **Game View viewport is focused**

This is implemented via an editor-managed input context:
- **context name**: `Editor.GameView`

Your scripts should register actions under that context so they are automatically gated.

### Creating actions and bindings
Namespace: `GameEngine.Scripting` (assembly `GameEngine.Input.ABI.dll`)

```csharp
using GameEngine.Scripting;

public static class Controls
{
    private const string Ctx = "Editor.GameView";

    // You can also pre-hash names via InputIds.HashInput.
    private const string ActionJump = "Game.Jump";

    public static void Setup()
    {
        // isAxis: false for digital actions
        Input.RegisterAction(Ctx, ActionJump, isAxis: false);

        // Bind space bar (key codes are the engine’s key enum values; see InputSystem key constants)
        Input.Bind(Ctx, ActionJump, Input.DeviceType.Keyboard, code: /* Space */ 32);
    }
}
```

### Polling action state

```csharp
using GameEngine.Scripting;

public static class Controls
{
    public static bool JumpPressedThisFrame()
    {
        Input.GetActionState("Game.Jump", out Input.ActionState s);
        return s.justPressed != 0;
    }
}
```

Notes:
- Polling returns `GE_Result_NotInitialized` if the native InputSystem is not available.
- Context stack operations are intentionally **not exposed** to gameplay scripts; the Editor owns them.

---

## ECS in C# (GameEngine.ECS.ABI)

Namespace: `GameEngine.ECS` (assembly `GameEngine.ECS.ABI.dll`)

### Get the world

```csharp
using GameEngine.ECS;

WorldHandle world = Ecs.PrimaryWorld;
```

### Built-in helpers (V1)

```csharp
uint e = world.CreateEmptyEntity("My Entity");
world.SetName(e, "Renamed");
string name = world.GetName(e);

// Transform as a 4x4 float matrix (column-major)
float[] m = world.GetTransformMatrix(e);
world.SetTransformMatrix(e, m);
```

### Custom ECS components from C# (V1: “blob components”)

You can store an `unmanaged` C# struct as a runtime-defined component type.
This is a **byte blob** component internally, but it participates in archetypes and queries.

```csharp
using System.Runtime.InteropServices;
using GameEngine.ECS;

[StructLayout(LayoutKind.Sequential)]
public struct PlayerStats
{
    public int score;
    public float health;
}

public static class Gameplay
{
    private static readonly ComponentType<PlayerStats> PlayerStatsType = new();

    public static void SetStats(WorldHandle world, uint entity, PlayerStats stats)
        => PlayerStatsType.Set(in world, entity, in stats);

    public static PlayerStats GetStats(WorldHandle world, uint entity)
        => PlayerStatsType.Get(in world, entity);
}
```

### Query entities with a component

```csharp
using GameEngine.ECS;

public static class Systems
{
    private static readonly ComponentType<PlayerStats> PlayerStatsType = new();

    public static void Tick(WorldHandle world)
    {
        using var q = world.CreateQuery(new uint[] { PlayerStatsType.TypeId });
        while (q.TryNext(out uint e))
        {
            var stats = PlayerStatsType.Get(in world, e);
            stats.health += 0.1f;
            PlayerStatsType.Set(in world, e, in stats);
        }
    }
}
```

---

## Making it work in the Editor

### Where scripts live
The Editor’s script compilation pipeline references all `GameEngine.*.ABI.dll` assemblies that are staged next to `Editor.exe`, including:
- `GameEngine.Scripting.ABI.dll`
- `GameEngine.Input.ABI.dll`
- `GameEngine.ECS.ABI.dll`

Place your gameplay scripts under the project `Assets/` folder (e.g. `Assets/MyGame.cs`).

### How input focus gating works (important)
- Actions registered under context **`Editor.GameView`** only receive input when:
  - Play Mode is active, and
  - the Game View viewport is focused

### Play Mode call order (V1)
- Enter:
  - world snapshot captured
  - play-mode undo stack isolated
  - runtime simulation systems enabled (physics/animation/audio emitter)
  - C# `[PlayModeEnter]` invoked
- Tick:
  - C# `[PlayModeTick]` invoked (only while playing; paused does not tick)
- Exit:
  - C# `[PlayModeExit]` invoked
  - audio stopped (`StopAllVoices`)
  - runtime simulation systems disabled
  - world snapshot restored
  - edit-mode undo history restored
  - **Change Review UI** (if user made undoable changes during Play)

