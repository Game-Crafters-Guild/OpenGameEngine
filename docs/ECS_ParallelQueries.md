# ECS Parallel Queries

Three of `Query`'s iteration modes run on the job system: `Parallel`,
`ParallelBatchEach` and `ParallelChunks`. Each has a sequential twin with the same
callback shape, so the choice of mode is a scheduling decision and not a rewrite.

| Parallel mode | Sequential twin | Callback |
|---|---|---|
| `Parallel(func, minBatchSize = 1000)` | `Each(func)` | `(EntityHandle, T&...)` |
| `ParallelBatchEach(func, minBatchSize = 0)` | `BatchEach(func)` | `(T*..., size_t)` or `(const EntityHandle*, T*..., size_t)` |
| `ParallelChunks(func)` | `ForEachChunk(func)` | `(T*..., size_t)` or `(T*...)` |

All three are **synchronous fork-join**: they dispatch through a job counter and
join before returning. When the world has no job system they fall through to
their sequential twin, so the same code runs in a headless test as in the editor.
Calling one from a worker thread is safe — the join participates and executes only
this query's batches, so it cannot deadlock against the pool.

## Declaring access

A query declares its intent per component. `Read<T>` binds the callback parameter as
`const T&`; `Write<T>` binds it as `T&`; an unqualified generic parameter is a compile
error. Reads never stamp a chunk's change version, writes always do — which is what
makes `Changed<>` filtering work, and why declaring a write you do not perform costs
every downstream consumer a wasted visit.

```cpp
#include "ECS/World.h"

world.Query<ECS::Write<Position>, ECS::Read<Velocity>>()
    .Parallel([dt](ECS::EntityHandle, Position& p, const Velocity& v) {
        p.x += v.x * dt;
        p.y += v.y * dt;
        p.z += v.z * dt;
    });
```

`Parallel` is the only mode that requires the entity handle. Its `static_assert` says
so directly: a component-only callback belongs on `Each` or `ParallelBatchEach`.

## Per-chunk callbacks

`ParallelBatchEach` gives the callback the chunk's raw column pointers and an element
count, which is the shape a vectorizing compiler wants. It is invoked once per chunk,
and several chunks may land on the same task.

```cpp
world.Query<ECS::Write<Position>, ECS::Read<Velocity>>()
    .ParallelBatchEach([dt](Position* pos, const Velocity* vel, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
        {
            pos[i].x += vel[i].x * dt;
            pos[i].y += vel[i].y * dt;
            pos[i].z += vel[i].z * dt;
        }
    });
```

The entity-aware form takes a parallel `const EntityHandle*` column first, indexed the
same way as the component columns — what a producer needs when it is recording which
entities it touched:

```cpp
world.Query<ECS::Read<Position>>()
    .ParallelBatchEach([&sink](const ECS::EntityHandle* entities,
                               const Position* pos, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            sink.Record(entities[i], pos[i]);
    });
```

`ParallelChunks` has the same pointer shape and also accepts a callback with no count
parameter. Use it when the work is naturally per-chunk rather than per-entity and you
do not want batch sizing in the way.

```cpp
world.Query<ECS::Read<Position>>()
    .ParallelChunks([&visited](const Position*, std::size_t count) {
        visited.fetch_add(count, std::memory_order_relaxed);
    });
```

## The four rules

**The functor is shared across every task.** One callback object serves all of them,
so a mutable capture is a data race unless it is an atomic or thread-local. The
counting examples above use `fetch_add` for exactly this reason.

**No structural changes during iteration.** Creating or destroying an entity, adding or
removing a component, compacting chunks or pruning empty archetypes on the same world
while a parallel query is in flight is undefined. Queue the work and apply it after the
join, or use the deferred command path.

**`Optional<T>` columns arrive as `nullptr`** when the archetype does not have the
component. The pointer callbacks hand you that null directly, so check before
dereferencing.

**Two entities in one chunk can be processed by one task, never by two.** Batching is
per chunk, so a callback never sees a partial chunk and two tasks never write the same
element.

## Batch size

`minBatchSize` is the smallest number of entities a task will be handed; chunks are
grouped until the batch reaches it. Pass `0` to take the policy default, which differs
per mode because the workloads differ:

- `Parallel` defaults to `QueryPolicy::MinBatchSizeDefault` (1000). The per-entity
  callback usually does enough work that small batches still amortize the dispatch.
- `ParallelBatchEach` defaults to `QueryPolicy::MinBatchSizeDefaultBatch` (10000). A
  per-chunk kernel is memory-bound and fine-grained batching is pure overhead.

Under job-system pressure the batch size is raised automatically — see
[ECS_QueryPolicy.md](ECS_QueryPolicy.md) for the backpressure thresholds and how to
reason about them. Pass an explicit value only when you have measured that the default
is wrong for your kernel.

## Skipping chunks nothing wrote

`Changed<T>(gate)` is honored by every iteration mode, parallel modes included. It
skips chunks whose `T` column has not been write-granted since the gate's version.

```cpp
ECS::ChangeGate gate;  // a member of the system, one gate per (system, query)

const uint64_t entrySample = world.GetGlobalSystemVersion();
auto q = world.Query<ECS::Read<Transform>, ECS::Write<WorldTransform>>();
q.Changed<Transform>(gate);
q.Parallel([](ECS::EntityHandle, const Transform& t, WorldTransform& wt) { /* ... */ });
gate.LastRunVersion = entrySample;
```

Two rules come with it. Sample `GetGlobalSystemVersion()` **before** iterating and write
that sample back as the next gate — an end-of-run resample can overtake a concurrent
writer's stamp and skip its entities permanently. And gate on a column you *read*, never
on one you *write*: writes stamp the chunks they visit, so `Changed<T>` over your own
`Write<T>` re-triggers itself forever.

## Choosing a mode

- Per-entity work that needs the handle, or that branches per entity: `Parallel`.
- A tight loop over columns, especially one you want vectorized: `ParallelBatchEach`.
- Work whose unit is the chunk rather than the entity: `ParallelChunks`.
- Small populations: measure before reaching for any of them. The sequential twin has
  no dispatch cost at all, and the fork-join only pays for itself once the callback
  work exceeds it.
