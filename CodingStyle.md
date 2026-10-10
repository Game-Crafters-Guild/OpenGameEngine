# Coding Style

This project adopts a consistent, modern C++ naming and API style across the entire repository. These conventions are canonical and apply to all modules (ECS, Engine, Rendering, UISystem, Assets, Tools, Examples, Tests). New code must follow them. Existing code will be migrated incrementally.

## Naming Conventions (Global)

- Functions/methods: PascalCase
  - Examples: ProcessCommands, GetEntityCount, Clear, View, Each, Parallel, Adaptive
- Private member variables: m_PascalCase
  - Example: m_CommandQueue, m_JobSystem
- Public data members (in plain-old-data structs/classes where appropriate): PascalCase (no m_ prefix)
  - Example: Position.X, PlayerStats.Health
- Local variables: camelCase (function scope) and parameters: camelCase
  - Example: entityCount, chunkSize, startIndex
- File-scope or static internal variables (module-private): s_PascalCase for static storage, camelCase for non-static
  - Example: s_ThorvgInitFlag, s_Inited, cacheSize, defaultWidth
- Local variables: camelCase
  - Example: entityCount, chunkSize
- Constants: kPrefix with PascalCase, at every scope — file, class and function-local
  alike. A named constant inside a function is still a constant, not a local variable.
  - Example: kMaxJobs, kDefaultBatchSize
- Types: PascalCase
  - Example: Entity, World, Archetype, Query, EntityHandle

## ECS Public API (Canonical)

Prefer these PascalCase entry points. Older camelCase names may still exist internally but should not be used in new code.

- World
  - ProcessCommands()
  - GetEntityCount() / GetArchetypeCount() / GetAllArchetypes()
  - Clear()
  - View<...>() returning a Query
  - Create(), CreateEntity(), CreateHandle(), CreateBatch(count), CreateBatchHandle(count)
  - SetBulkComponents(handles, components)
  - DestroyEntity(...), DestroyEntityImmediate(...)
  - ReserveEntities(count), ReserveArchetypeCapacity<...>(count)

- Query
  - Each(lambda)
  - Parallel(lambda, minBatchSize)
  - ForEachChunk(lambda)
  - ParallelChunks(lambda)
  - Adaptive(lambda) and adaptive(lambda) both exist; prefer Adaptive in examples going forward
  - Count()
  - Filters: With<...>(), Without<...>() — one call each, naming every type

- Entity (fluent API)
  - Set<T>(), Get<T>(), Has<T>(), Remove<T>()
  - Enable(), Disable(), IsEnabled()
  - Destroy(), Clone(), IsValid(), GetHandle()

## Migration Guidance

- Use PascalCase in all user-facing code (Examples, Tests, tools). If a PascalCase alias does not exist yet, add it alongside the existing camelCase implementation.
- Do not maintain duplicate semantics long-term; once usage is updated and tested, remove the camelCase entry.
- Avoid verbose logging; reserve logs for errors/warnings or debugging.

## Formatting

- Brace style: Allman or project default from .editorconfig/IDE settings.
- Keep functions short and focused; prefer small helpers over giant functions.
- Avoid public data members in complex objects; encapsulate behind accessors. For simple POD/aggregate types used as components or data carriers, public fields are allowed and should be PascalCase (no m_ prefix).


## Headers, Includes, and Organization

- One class or primary type per file where practical
- Section order: public, protected, private
- Prefer forward declarations in headers; include minimal headers, use includes in .cpp
- Include order: own header, related module headers, standard library, third-party
- Use angle brackets for external libraries and quotes for project headers

## Lambdas

- A lambda is a short adapter passed to an API: an `Each` body, a sort key, a small event handler. It captures what it needs by name.
- A lambda that grows past a handful of lines, captures `[&]` across unrelated state, is assigned to a local and called once, or is needed twice becomes a named function (file-local in an anonymous namespace, or a member) in the file that owns the concept.
- Do not build a function out of nested local lambdas; the reader has to hold every capture in mind to follow the control flow.

## Namespaces

- Code should reside in project namespaces; no global-scope symbols in headers
- Do not use `using namespace` in headers; prefer narrow using declarations in .cpp only

## Enums

- Prefer `enum class` for type safety; use explicit underlying types (e.g., `enum class Foo : uint32_t`) when data size/layout matters
- For plain C-compatible flags/bitfields where needed, document the rationale and ensure explicit widths

## Literals and Types

- Use fixed-width integer types where layout matters (uint32_t, int16_t, etc.)
- Prefer std::chrono for time and std::filesystem for paths

## Nulls, Returns, and Control Flow

- Use nullptr, not 0 or NULL
- Return early to avoid deep nesting; keep hot paths straight-line when possible

## Error Handling

- In hot paths avoid exceptions; prefer lightweight validation and early returns
- In non-hot paths/tools, exceptions are acceptable with clear ownership and error contracts

## Concurrency

- Document thread affinity and ownership in public APIs
- Prefer immutability; otherwise protect with clear synchronization primitives

## Templates and Concepts

- Constrain public templates with concepts or requires clauses where practical
- Keep large template definitions in .inl files for clarity

## Move Semantics and Copies

- Provide move operations for heavy types; delete or define copy semantics intentionally

## Logging

- Minimal logging in hot paths; warnings/errors only. Avoid verbose enter/exit logs.

## Comments and Documentation

- Doxygen-style comments for public APIs
- Examples and docs should use canonical PascalCase API only

## Assertions

- Prefer project Assert macro over raw assert; avoid side effects in assertions

## Unit Tests

- Test names should state behavior, not implementation
- For ECS, include performance sanity checks for hot paths

## Threading and Jobs

- Prefer job system helpers and Query::Parallel/ParallelChunks for data-parallel work (synchronous — they join internally).
- For explicit fork-join, use JobSystem::JobCounter: Run(fn, counter) to submit, Wait(counter) to join.

## Error Handling

- Use assertions in tests; in runtime code, prefer lightweight validation and early returns over exceptions in hot paths.

## Documentation

- Add brief comments for non-obvious behavior and any public API that could be misused.
- Update this guide if you introduce a new, widely used pattern.

