# C# ECS Systems Architecture

## Overview

This document describes the architecture for exposing ECS system and component authoring to C# script users, enabling high-performance per-entity iteration from managed code without per-entity ABI transitions.

Two complementary abstractions are provided:

| Abstraction | Purpose | Entity Iteration? | Generated Entity Iteration? |
|---|---|---|---|
| **`GameSystem`** | Global per-frame logic (spawners, state machines, managers) | No — manual ECS access if needed | No |
| **`IEntitySystem`** | Per-entity component processing | Yes — source-generated chunk iteration | Yes |

Both participate in the same managed update loop. Generated module initializers register
entity systems; `GameSystem` discovery also has a reflection fallback.

---

## Naming & Namespace

```
GameEngine.Scripting.GameSystem          — abstract base class
GameEngine.Scripting.IEntitySystem       — interface for per-entity systems
GameEngine.Scripting.IComponent          — marker interface for user components
GameEngine.Scripting.BuiltInComponentAttribute — binds a mirror to a native component name
GameEngine.Scripting.WithoutAttribute    — excludes component types from a query
```

All user-facing types live in `Managed/Scripting.ABI/` (the public API surface). Internal runtime plumbing lives in `Managed/ECS.ABI/` and `Managed/Scripting.Runtime/`.

### Assembly Layout

```
Managed/Scripting.ABI/         — Public types: IEntitySystem, GameSystem, IComponent, WithoutAttribute
                                 (user-facing API, referenced by user scripts)
Managed/ECS.ABI/               — Internal ECS plumbing: ChunkQuery, P/Invoke wrappers
                                 (not user-facing, referenced by Scripting.Runtime)
Managed/Scripting.Runtime/     — GameSystemRunner, system discovery, tick dispatch,
                                 [UnmanagedCallersOnly] exports for native bridge
                                 (ships in BOTH editor and standalone builds)
Managed/Editor.Managed/        — PlayModeDriver hard-wires calls to Scripting.Runtime
                                 (editor-only, NOT shipped in builds)
Managed/SourceGenerators/      — Roslyn IIncrementalGenerator for IEntitySystem
                                 (compile-time only, .NET Standard 2.0)
```

Key: `Scripting.Runtime` replaces the original plan of putting `GameSystemRunner` in
`Editor.Managed`. This assembly ships in builds and provides the exported entry points
that the native `ManagedSystemBridge` calls.

---

## IEntitySystem

### User-Facing Contract

```csharp
/// <summary>
/// Implement on a partial struct to define a per-entity ECS system.
/// A source generator emits the chunk iteration boilerplate.
/// </summary>
public interface IEntitySystem
{
    /// <summary>Execution order. Lower runs first. Default 0.</summary>
    int Order => 0;

    /// <summary>Whether this system is currently enabled.</summary>
    bool Enabled => true;
}
```

The user writes a `partial struct` implementing `IEntitySystem` with an `Execute` method whose parameters declare the component access pattern:

```csharp
public partial struct MovementSystem : IEntitySystem
{
    void Execute(ref Position pos, in Velocity vel, float deltaTime)
    {
        pos.X += vel.X * deltaTime;
        pos.Y += vel.Y * deltaTime;
        pos.Z += vel.Z * deltaTime;
    }
}
```

### Excluding Components

Use `[Without]` to exclude entities that have a specific component:

```csharp
[Without(typeof(Static), typeof(Disabled))]
public partial struct MovementSystem : IEntitySystem
{
    void Execute(ref Position pos, in Velocity vel, float deltaTime) { ... }
}
```

### Parameter Convention

| Parameter pattern | Meaning |
|---|---|
| `ref T` | Read-write access to component T (required) |
| `in T` | Read-only access to component T (required) |
| `float deltaTime` | Injected frame delta time |
| `uint entityId` | Injected entity ID for current entity |

All component types must be `unmanaged` structs implementing `IComponent`.

### Component Struct Rules

- Must be `unmanaged` (no managed references — no `string`, `object`, arrays, classes)
- Must use `[StructLayout(LayoutKind.Sequential)]`
- **No `bool` fields** — use `byte` instead (C#/C++ `bool` size can differ)
- **Enums must have explicit backing type** matching C++ side (e.g., `: int`, `: byte`)
- Safe types: `float`, `double`, `int`, `uint`, `short`, `ushort`, `byte`, `sbyte`, `long`, `ulong`
- No `nint`/`nuint` (pointer-sized, not portable)

### What the Source Generator Emits

The generator adds a companion partial struct with these entry points:

1. `[ModuleInitializer] __RegisterSystem()` registers execution and cleanup callbacks
   with `GameSystemRunner` when the assembly loads.
2. `__EnsureTypeIds()` resolves component IDs lazily on the first execution. IDs are
   `ulong`; built-in IDs must be resolved after native components are registered.
3. `__Execute(ulong worldHandle, float deltaTime)` creates a cached query, resets it,
   and visits each matching archetype and chunk. It obtains writable spans for `ref`
   parameters and read-only spans for `in` parameters, then calls the user method
   for each matching entity.
4. `__Destroy()` destroys the cached query handle.

### Key Design Decisions

A default system struct is created for each generated execution call, so instance
fields are not persistent system state. Store persistent per-entity state in
components or global state in a `GameSystem`.

Query and component IDs are cached between executions. Generated iteration makes
ABI calls to acquire chunk spans; the per-entity loop itself is managed code over
native memory. Calls made by the user's `Execute` method can still cross the ABI.

The generated file is the authoritative expansion for a particular system; inspect
it through the IDE's generated sources instead of maintaining a handwritten copy.

---

## GameSystem

### User-Facing Contract

```csharp
/// <summary>
/// Base class for global per-frame systems (spawners, managers, state machines).
/// Not source-generated. Access ECS manually if needed.
/// </summary>
public abstract class GameSystem
{
    /// <summary>The primary ECS world.</summary>
    protected internal GameEngine.ECS.WorldHandle World { get; internal set; }

    /// <summary>Enable/disable this system at runtime.</summary>
    public bool Enabled { get; set; } = true;

    /// <summary>Execution order. Lower runs first. Default 0.</summary>
    public virtual int Order => 0;

    public virtual void OnCreate() { }
    public virtual void OnEnable() { }
    public virtual void OnUpdate(float deltaTime) { }
    public virtual void OnDisable() { }
    public virtual void OnDestroy() { }
}
```

### Example

```csharp
public class WaveSpawner : GameSystem
{
    private float timer;
    private int waveCount;

    public override void OnUpdate(float deltaTime)
    {
        timer += deltaTime;
        if (timer > 5.0f)
        {
            timer = 0;
            waveCount++;
            for (int i = 0; i < waveCount * 3; i++)
            {
                uint entity = World.CreatePrimitive(PrimitiveType.Sphere, $"Enemy_{i}");
                // Set components...
            }
        }
    }
}
```

---

## IComponent

### User-Facing Contract

```csharp
/// <summary>
/// Marker interface for user-defined ECS components.
/// Must be an unmanaged struct with sequential layout.
/// </summary>
public interface IComponent { }
```

### Example

```csharp
[StructLayout(LayoutKind.Sequential)]
public struct Position : IComponent
{
    public float X, Y, Z;
}

[StructLayout(LayoutKind.Sequential)]
public struct Velocity : IComponent
{
    public float X, Y, Z;
}

[StructLayout(LayoutKind.Sequential)]
public struct Health : IComponent
{
    public float Current;
    public float Max;
}
```

### Rules (enforced by source generator diagnostics):

- Must be `unmanaged` (no managed references)
- Must be a `struct`
- Must implement `IComponent`
- Should have `[StructLayout(LayoutKind.Sequential)]` (generator emits warning if missing)
- No padding ambiguity: use explicit field sizes (float, int, etc.)

---

## Runtime Architecture

### Execution Flow

The editor's `PlayModeDriver` and the standalone `ManagedSystemBridge` dispatch to
`GameSystemRunner` in `Managed/Scripting.Runtime/`. The runner owns initialization,
ordered execution, and shutdown of both system kinds.

### Unified Ordering

`GameSystem` and `IEntitySystem` entries share an execution order. The `Order`
property supplies numeric priority; explicit `[Before]` and `[After]` dependencies
also participate in sorting. See `GameSystemRunner.cs` for cycle handling.

### Structural Change Fencing

`GameSystemRunner.Tick()` enables deferred structural changes before executing
systems. Its `finally` block flushes deferred commands and clears the deferral
flag, so a system exception does not leave deferral enabled. The standalone bridge
also fences its managed dispatch.

Keep component spans within the generated iteration. Entity creation, destruction,
and component changes can invalidate native storage after commands are processed.

### Managed Dispatch

The runner dispatches enabled systems from its published execution snapshot.
Generated entity iteration acquires spans per chunk, without a native call for
merely advancing to the next entity. Manual ECS calls inside a system still have
their own ABI cost.

### GameSystemRunner (Internal)

The implementation is in `Managed/Scripting.Runtime/GameSystemRunner.cs`.
`GameSystemRunnerExports.cs` provides the standalone native entry points. Keep
user logic in `GameSystem` callbacks or `IEntitySystem.Execute`; these runtime
entry points are infrastructure.

### System Discovery

The generator emits `[ModuleInitializer]` registration for entity systems and
eligible `GameSystem` subclasses. `GameSystemRunner` also discovers unregistered
`GameSystem` subclasses through reflection where dynamic code is available.
Registrations survive play-mode cycles; registrations owned by an unloading
assembly context are purged.

---

## Built-in C++ Component Access

C# systems can directly iterate over registered C++ engine components through
`[BuiltInComponent]`. Use the supplied `GameEngine.Scripting.Transform` mirror for
`GameEngine::Components::Transform`; it stores the same 64-byte, column-major matrix.
Do not redeclare that mirror. The `Position` struct in the earlier example is a
user component, not a built-in native component.

```csharp
using GameEngine.Scripting;

// Transform is the engine-provided native mirror. Velocity is the
// user component declared in the earlier component example.
public partial struct MoveNativeSystem : IEntitySystem
{
    public void Execute(ref Transform transform, in Velocity velocity, float deltaTime)
    {
        transform.Translate(velocity.X * deltaTime,
                            velocity.Y * deltaTime,
                            velocity.Z * deltaTime);
    }
}
```

The source generator detects `[BuiltInComponent]` and emits `Ecs.GetComponentTypeIdByName`
instead of `Ecs.RegisterBlobComponent`, resolving to the same type ID the C++ engine uses. The attribute must name an
existing native registration exactly, and the mirror must match its size and layout;
it does not create a native component from an arbitrary name.

### Chunk-Based Iteration

Each archetype owns an `ArchetypeTable` with colocated component columns. Native
and managed blob components share the same entity boundaries within each chunk.
The generator walks chunk indices and obtains each required component span for
that chunk; it no longer advances independent component slices by entity offset.

Chunks target 16 KiB, but capacity depends on the entire archetype signature,
entity handles, and alignment. Oversized rows can require larger chunks. Do not
calculate capacity from `sizeof(T)` for one component alone or assume managed
components occupy one flat chunk.

### Available Built-in Mirrors

`Managed/Scripting.ABI/BuiltInComponents.cs` supplies these mirrors:

| C# Type | C++ Type | Size | Storage |
|---|---|---|---|
| `Transform` | `GameEngine::Components::Transform` | 64 bytes | 16 column-major floats |
| `WorldTransform` | `GameEngine::Components::WorldTransform` | 68 bytes | 16 column-major floats and a 32-bit version |

`Position`, `Velocity`, `Rotation`, and `Scale` are not supplied built-in mirrors.
A user-defined component with one of those names remains a separate component.

---

## Chunk Query ABI (Internal Plumbing)

### Storage Model

`Engine/Modules/ECS/Include/ECS/ArchetypeTable.h` defines the shared chunk layout.
Cached queries enumerate matching archetypes and chunks, and refresh against
structural changes. They must be destroyed when their owning system shuts down.

### C++ Side

Use the declarations in `Engine/Include/Scripting/ECSABI.h`. Component type IDs use
`GE_ECS_ComponentTypeId` (`uint64_t`); entity IDs remain 32-bit. The cached-query
functions include `CachedQueryCreate`, `CachedQueryReset`, `CachedQueryGetArchetypeInfo`,
`CachedQueryGetChunkData`, and `CachedQueryDestroy`, all with the `GE_ECSABI_` prefix.

Check each function's `GE_Result` before consuming its outputs. Handles must refer
to live engine objects; an arbitrary nonzero integer is not a valid query handle.

### C# Side

`Managed/ECS.ABI/Internal/ChunkQueryNative.cs` exposes the wrappers used by generated
code. Required and excluded component lists use `ReadOnlySpan<ulong>`; component
arguments are `ulong`. The wrappers return typed spans over the current native
chunk. User systems should normally use generated `Execute` parameters instead
of managing these query handles directly.

---

## Source Generator Architecture

### Location

`Managed/SourceGenerators/EntitySystemGenerator/` — a standalone Roslyn analyzer/generator
project targeting **.NET Standard 2.0** (Roslyn requirement for source generators).

### Integration Paths

The generator must work in two contexts:

1. **MSBuild / IDE** — referenced as an `<Analyzer>` in the user's `.csproj`. VS/Rider
   run it automatically, providing intellisense and error squiggles for generated code.

2. **CompileServerHost** — loaded explicitly via `CSharpGeneratorDriver`:
   ```csharp
   var generator = LoadGeneratorFromAssembly(generatorPath);
   var driver = CSharpGeneratorDriver.Create(generator);
   driver = driver.RunGeneratorsAndUpdateCompilation(
       compilation, out var outputCompilation, out var diagnostics);
   var emitResult = outputCompilation.Emit(peStream, pdbStream);
   ```
   The `GeneratorDriver` instance is cached in `WorkspaceState` for incremental builds.

### What It Generates

For each `partial struct` implementing `IEntitySystem` with an `Execute` method:

1. **`[ModuleInitializer]` registration method** — registers system callbacks with
   `GameSystemRunner`. Component type IDs are resolved lazily on first execution.
2. **`__Execute(ulong world, float dt)` static method** — the chunk iteration loop.
3. **`__Destroy()` static method** — destroys cached query handle on shutdown/hot-reload.
4. **Diagnostic errors** for invalid signatures.

### Diagnostic IDs

| ID | Severity | Description |
|---|---|---|
| `GE0001` | Error | IEntitySystem must be a partial struct |
| `GE0002` | Error | Execute method not found |
| `GE0003` | Error | Component parameter type is not unmanaged |
| `GE0004` | Error | Component parameter type does not implement IComponent |
| `GE0005` | Warning | Component struct missing [StructLayout(LayoutKind.Sequential)] |
| `GE0006` | Error | Execute must have at least one component parameter |
| `GE0007` | Warning | Execute has no deltaTime parameter (unusual) |
| `GE0008` | Error | Multiple Execute methods found (overloads not supported) |
| `GE0009` | Error | Execute must not be generic |
| `GE0010` | Error | Execute must be an instance method (not static) |
| `GE0011` | Error | Execute must not be async |
| `GE0012` | Error | Component parameter passed by value (must be `ref` or `in`) |
| `GE0013` | Error | IEntitySystem must not be a nested type |
| `GE0014` | Warning | Component struct contains `bool` field (use `byte` instead) |
| `GE0015` | Error | Same component type appears multiple times in Execute parameters |
| `GE0016` | Warning | IEntitySystem struct declares instance fields (zero-initialized each frame) |

---

## Hot-Reload Behavior

Generated module initializers register systems when a new assembly loads. The
runtime purges registrations associated with the unloading assembly context and
reconciles the new domain's systems. Do not rely on instance fields surviving a
reload. Native world data and system-local managed state have separate lifetimes.

### Component Layout Changes

Generated component schemas describe field names, types, offsets, and defaults.
`GE_ECSABI_RegisterBlobComponentWithSchema` supports field-aware migration of
existing blob components, including same-size field reorders and size changes.
Diagnostics warn about fields or layouts that cannot be exported in the schema;
those fields are excluded from inspection, persistence, and migration.

The older name-and-size-only `RegisterBlobComponent` path cannot perform this
schema migration. Treat registration failures as errors; re-entering play mode is
not a general repair for an incompatible layout. Native built-in mirrors must
continue to match their C++ definitions.

---

## Build Pipeline (NativeAOT)

Use the supported [game publishing workflow](../GamePublishing.html) for standalone
packaging. The runtime includes native entry points for managed-system dispatch,
and generated registrations avoid relying solely on reflection discovery. An
arbitrary `dotnet publish` command is not a complete engine packaging recipe.

---

## Integration with Existing Play Mode

`PlayModeDriver` (in `Editor.Managed`) **hard-wires** calls to `Scripting.Runtime`'s
`GameSystemRunner` — no `[PlayModeTick]` attribute discovery needed for the bootstrap
itself. This avoids the issue where `PlayModeDriver.IsCandidateAssembly()` skips
`GameEngine.Editor.Managed` assemblies.

```csharp
// In PlayModeDriver.cs (Editor.Managed) — direct calls, not attribute-based
internal static class PlayModeDriver
{
    public static int OnEnter()
    {
        // ... existing hook discovery ...
        var world = Ecs.PrimaryWorld;
        GameSystemRunner.Initialize(world.Handle);
        // ... invoke [PlayModeEnter] hooks ...
    }

    public static int Tick(float deltaSeconds)
    {
        GameSystemRunner.Tick(deltaSeconds);
        // ... invoke [PlayModeTick] hooks ...
    }

    public static int OnExit()
    {
        // ... invoke [PlayModeExit] hooks ...
        GameSystemRunner.Shutdown();
    }
}
```

For standalone builds, the engine calls `GameSystemRunnerExports.NativeInitialize/NativeTick/
NativeShutdown` directly from the native `ManagedSystemBridge` C++ ISystem via the
`[UnmanagedCallersOnly]` exports in `Scripting.Runtime`.

---

## Performance Characteristics

Generated iteration cost depends on the number of matching archetypes, chunks,
and component spans, plus the user's work. Component size affects chunk capacity
through the complete archetype layout. There is no fixed transition count for a
given entity count alone.

The legacy per-entity `QueryNext` API is not the current iteration contract. Use
Release builds and a representative workload when comparing performance; this
architecture does not imply a fixed speedup.

### SIMD / Auto-Vectorization

`Span<Position>` contains contiguous `Position` structs, each with its own X, Y,
and Z fields. Vectorization depends on the generated code, runtime, and operation;
inspect or measure the specific loop before relying on it.
