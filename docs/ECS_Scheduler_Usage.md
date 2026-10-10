# ECS Scheduler Usage and API Choices

This guide explains the available iteration APIs (Each, Parallel, ForEachChunk, ParallelChunks), the new Scheduler helpers and fluent SystemJob API, and when to use which.

## Iteration APIs on Query<T...>
- Each(fn):
  - Signature: fn(EntityHandle, Arg<Ts>...)
  - Arg<T> is T& for required qualifiers, T* for Optional.
  - Best for simple per-entity logic; no job system required.
- Parallel(fn, minBatchSize=1000):
  - Same signature as Each; uses the World’s Job System if available; coalesces work per chunk with adaptive batching from QueryPolicy.
  - Good for medium data sizes; keeps per-task overhead low while parallelizing.
- ForEachChunk(fn):
  - Signature: fn(T*..., size_t count) — Optional<T> may be nullptr
  - Best for high-performance code with tight inner loops and SIMD-friendly processing.
- ParallelChunks(fn):
  - Same as ForEachChunk but submits one task per chunk to the Job System.
  - Use when data sets are very large and chunk-level work dominates overhead.

## Scheduler helpers and SystemJob (fluent)
- MakeJobEach/Parallel/ForEachChunk/ParallelChunks: wrap queries to produce JobDesc with read/write sets
- SystemJob::Each/Parallel/ForEachChunk/ParallelChunks: fluent builder with Name/WithName and DependsOn
  - Example: Scheduler::SystemJob::Parallel(q, fn, 0).WithName("WriteVel").DependsOn(0);

All helpers copy read/write sets from the query (GetReadSet/GetWriteSet), so the scheduler can avoid conflicts when running jobs together.

## Conflict-free scheduling and dependencies
- The scheduler forms parallel waves of jobs that do not conflict (no write-write or read-write overlap by DenseSignature).
- Optional dependency edges can be provided to enforce ordering across jobs (Dependency{before, after} or SystemJob.DependsOn).
- Typical patterns:
  - Multiple read-only jobs on disjoint or same components can run together.
  - Any writer to a component forces sequencing with readers/writers of that component.
  - Use dependencies when semantic ordering is required even without data hazards.

## When/why to choose each API
- Each vs Parallel:
  - Use Each for small counts or when no Job System is present.
  - Use Parallel for medium counts; let QueryPolicy choose batch sizes (via World.SetQueryPolicy).
- ForEachChunk vs ParallelChunks:
  - Use ForEachChunk for single-threaded, cache-efficient processing and when you want explicit control over loops.
  - Use ParallelChunks for very large data with naturally chunkable work; beware of per-chunk overhead.
- Scheduler vs direct Query calls:
  - Direct calls are simplest when you have a few independent operations.
  - Use the Scheduler when you have multiple jobs with potential conflicts and want the runtime to parallelize safely.
  - Prefer helpers or SystemJob to reduce boilerplate and ensure read/write sets are correct.

## Policy configuration
- World owns QueryPolicy; WorldBuilder.SetQueryPolicy(policy) applies it at construction.
- Fields:
  - MinBatchSizeDefault: default for Parallel when caller passes 0
  - EnableBackpressure, QueuePressureFactor/PendingPressureFactor, PressuredMinBatchFloor: adapt batch sizes when the worker queue is pressured

## Minimal example
```cpp
WorldBuilder builder;
QueryPolicy policy; policy.MinBatchSizeDefault = 2000;
auto world = builder.SetJobSystem(&js).SetQueryPolicy(policy).Build();

auto qA = world->Query<Read<Pos>>();
auto qB = world->Query<Write<Vel>>();
Scheduler sched(world->GetJobSystem());

auto jA = Scheduler::MakeJobEach(qA,  [&](EntityHandle, const Pos&){}, "ReadPos");
auto jB = Scheduler::MakeJobEach(qB,  [&](EntityHandle, Vel& v){ v.x+=1; }, "WriteVel");

std::vector<Scheduler::JobDesc> jobs{ jA, jB };
sched.Run(jobs);

// Fluent SystemJob
std::vector<Scheduler::SystemJob> sysJobs;
sysJobs.push_back(Scheduler::SystemJob::Each(qA, [&](EntityHandle, const Pos&){}).WithName("ReadPos"));
sysJobs.push_back(Scheduler::SystemJob::Parallel(qB, [&](EntityHandle, Vel& v){ v.x+=1; }, 0).WithName("WriteVel").DependsOn(0));
sched.Run(sysJobs);
```

## Comparison quick table
- Each: simple, single-threaded
- Parallel: auto-batched parallelism; uses QueryPolicy; same signature as Each
- ForEachChunk: chunk pointer access; max control and perf; single-threaded
- ParallelChunks: one task per chunk; for very large datasets
- Scheduler + helpers/SystemJob: assemble multiple jobs; avoid conflicts; optional dependencies

## Diagnostics (compile-time flag)

## Chunk lambda signatures and Optional semantics
- ForEachChunk and ParallelChunks support two callable signatures:
  - fn(T*..., size_t count)
  - fn(T*...)
- With Optional<U> in the query, the U* pointer passed to the chunk lambda may be nullptr for chunks that do not contain U.
- Performance characteristics:
  - The adapter that allows omitting the count adds negligible overhead in typical builds and is verified by tests.
  - For Optional components, branches on nullptr are per-chunk, not per-entity, and remain efficient.

## Adapter diagnostics
- Define ECS_ADAPTER_DIAGNOSTICS=1 when configuring tests to enable adapter counters:
  - Detail::ChunkAdapterDiag::CallsWithCount
  - Detail::ChunkAdapterDiag::CallsWithoutCount
- Tests in QueryBenchmarks log these counters when the flag is enabled.

- Define ECS_SCHEDULER_DIAGNOSTICS=1 to enable wave-level timing and totals printed to stdout.
- Use this to tune QueryPolicy and job composition.

## Notes
- Optional<T> in Queries passes T* which can be nullptr for missing data in chunk APIs.
- MakeJobParallel/ParallelChunks helpers internally wait on their jobs, so Scheduler treats them as single wave items without leaking handles.

## Examples
- Tests/ECS/ParallelPerfTest.cpp — MakeJobParallel
- Tests/ECS/ECSCoreTest.cpp — Scheduler::SystemJob chunk jobs with Optional


