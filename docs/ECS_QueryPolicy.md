# ECS QueryPolicy and Backpressure-Aware Parallelism

This document explains how ECS queries adapt their task granularity based on runtime load, and how to configure the defaults through QueryPolicy.

## QueryPolicy

QueryPolicy defines conservative defaults for batch sizing when using Query::Parallel. These values were chosen from micro-benchmarks and are intended as safe starting points across machines.

Fields:
- `MinBatchSizeDefault` (1000): minimum batch size when none is passed to `Query::Parallel`
- `MinBatchSizeDefaultBatch` (10000): the same for `Query::ParallelBatchEach`, whose per-chunk callback is memory-bound and pays more for fine batching
- `EnableBackpressure` (true): adapt the batch size to job-system pressure
- `QueuePressureFactor` (8): when queue length exceeds workers * factor, raise the batch size
- `PendingPressureFactor` (16): when pending tasks exceed workers * factor, raise the batch size
- `PressuredMinBatchFloor` (32768): the floor the batch size is raised to under pressure

## Behavior

When you call Query::Parallel(func, minBatchSize):
- If minBatchSize == 0, the default comes from `QueryPolicy::MinBatchSizeDefault`
  (`Query::ParallelBatchEach` takes `MinBatchSizeDefaultBatch` instead).
- If backpressure is enabled, the job system metrics are sampled:
  - qsize = `GetApproximateQueueSize()`
  - pending = `GetPendingTasksApprox()`
  - workers = `GetWorkerCount()`
- If qsize > workers * `QueuePressureFactor` OR pending > workers * `PendingPressureFactor`, then
  - minBatchSize = max(minBatchSize, `PressuredMinBatchFloor`)

This increases batch size under load to reduce task scheduling overhead.

## Notes
- `WorldBuilder::SetQueryPolicy` sets it at world construction; after that
  `World::SetQueryPolicy` replaces it wholesale. There is no per-field setter.
- Backpressure adjustments only ever increase the batch size, so they cannot oscillate.
- For the iteration modes these numbers apply to, see [ECS_ParallelQueries.md](ECS_ParallelQueries.md).

