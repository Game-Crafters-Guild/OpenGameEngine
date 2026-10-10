# Vendored ECS framework versions

Benchmark case shapes are modeled on https://github.com/abeimler/ecs_benchmark
(the repo itself is NOT built here; frameworks are vendored as amalgamated
single-file distributions and the harness is plain std::chrono — no
google-benchmark dependency).

| Framework | Version tag | Source | Files | Licence |
|-----------|-------------|--------|-------|---------|
| EnTT | `v3.16.0` | https://raw.githubusercontent.com/skypjack/entt/v3.16.0/single_include/entt/entt.hpp | `third_party/entt/entt.hpp` | MIT, `third_party/entt/LICENSE` |
| flecs | `v4.1.6` | https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.h and `distr/flecs.c` | `third_party/flecs/flecs.h`, `third_party/flecs/flecs.c` | MIT, `third_party/flecs/LICENSE` |
| gaia-ecs | `v0.9.2` | https://raw.githubusercontent.com/richardbiely/gaia-ecs/v0.9.2/single_include/gaia.h | `third_party/gaia/gaia.h` | MIT, `third_party/gaia/LICENSE` |
| GameEngine ECS | this tree (`Engine/Modules/ECS`) | in-repo | linked as the `ECS` CMake target | this repository |

Downloaded 2026-07-12. Each `LICENSE` is the upstream repository's `LICENSE`
at the same tag, copied verbatim.

`gaia.h` inlines a span implementation based on https://github.com/tcbrindle/span,
which is under the Boost Software License 1.0. `third_party/gaia/LICENSE_1_0.txt`
is that repository's `LICENSE_1_0.txt` at commit `836dc6a0ef` (it has no tags;
the file has not changed since it was added in `88b38fa093`), copied verbatim.

## Fairness rules

- Release config (`/O2`) for every TU including `flecs.c`; `/arch:AVX2` applied
  uniformly to all lanes (it is the shipped baseline of the `ECS` module).
- Single-threaded documented idiom per framework:
  - ours: `World::CreateHandle(comps...)`, `DestroyEntityImmediate`,
    `AddComponentImmediate`/`RemoveComponentImmediate`,
    `World::Query<Read<...>, Write<...>>().Each(...)`
  - EnTT: `registry.create()` + `emplace`, `registry.destroy`,
    `view<...>().each(...)`
  - flecs: `world.entity().set(...)`, `entity.destruct()`, cached queries
    (`query_builder<...>().cache_kind(QueryCacheAuto).build()`) `.each(...)`
  - gaia-ecs: `world.add()` + `world.add<T>(e, value)`, `world.del`,
    `world.query().all<...>().each(...)`
- Query/view objects are constructed once per case, outside the timed region,
  for all frameworks.
- Identical component layouts everywhere: `Position{float,float}`,
  `Velocity{float,float}`, `DataComp{int32,float}`, `Health{int32}`,
  `Damage{int32}`.
- `std::chrono::steady_clock`; median of 5 timed runs after 2 warmups for
  structural cases (create/destroy/add-remove/random-get), median of 9 timed
  runs after 3 warmups for per-frame iteration cases.
- Deterministic seed (`0x5EED`) for the shuffled random-access order.
- Rows with framework `ours-extra` (Changed<> gated iteration, BatchEach,
  ParallelBatchEach, CreateBatchHandle) are differentiating features measured
  for context — they are NOT part of the apples-to-apples comparison.
