# ECS Immediate vs Deferred Operations and Reserving Capacity

This short guide explains when to use immediate vs deferred operations and how to pre-reserve capacity for better performance.

## Immediate vs Deferred

- Deferred (default):
  - Operations such as Entity.Set(...), Entity.Remove<...>(), and World.AddComponent(...) enqueue commands that are applied by World.ProcessCommands().
  - Pros: Batching reduces contention and allows structural changes to be coalesced.
  - Cons: Effects are not visible until ProcessCommands(), so reads (Get/Has/View iteration) must consider that.

- Immediate:
  - Use the explicit Immediate APIs when available (e.g., DestroyEntityImmediate) or the direct/bulk mode to bypass the command buffer.
  - For bulk updates/streaming scenarios:
    - World.BeginBulkOperations()
    - World.CreateDirect() for fast handle creation without enqueuing
    - World.SetComponentDirect(entity, component)
    - World.EndBulkOperations()
  - Pros: Deterministic timing; avoids command-buffer capacity constraints.
  - Cons: Less batching; be careful with thread-safety and contention.

Tip: Prefer deferred for general gameplay code; use direct/bulk for import, large streaming, or tight loops where you control ordering.

## Reserving Capacity

- ReserveEntities(count):
  - Pre-allocates entity metadata to reduce growth overhead.

- ReserveArchetypeCapacity<Components...>(count):
  - Ensures that the archetype for the specified component set has at least 'count' capacity allocated.
  - This avoids costly reallocation when rapidly creating many matching entities or moving entities into that archetype.

Example:

```cpp
World world;

// Reserve global entity metadata
world.ReserveEntities(100000);

// Reserve capacity for an archetype (Position+Velocity)
world.ReserveArchetypeCapacity<Position, Velocity>(50000);

// Bulk creation path
world.BeginBulkOperations();
for (int i = 0; i < 50000; ++i) {
    auto h = world.CreateDirect();
    world.SetComponentDirect(h, Position{float(i), 0, 0});
    world.SetComponentDirect(h, Velocity{1, 0, 0});
}
world.EndBulkOperations();
```

Notes:
- Reserve calls are hints; they do not change semantics.
- Direct operations should be used in carefully controlled sections to avoid data races while other systems iterate.

