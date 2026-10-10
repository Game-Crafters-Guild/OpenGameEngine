# GameEngine.HotReload

Lightweight hot reload subsystem: collectible AssemblyLoadContext management,
preload/swap, token-based export invocation, and unload/leak verification.

Script compilation is owned by `Managed/CompileServerHost` (resident Roslyn
service reached over a named pipe); this assembly does not compile code.

## Quick Links
- Best practices: ../../docs/Scripting/HotReload_BestPractices.md

## Notes
- Old contexts are unloaded with a bounded background verification loop; a context
  that survives is reported loudly and counted in `GetLeakMetrics`/`GetStatus`.
- `[InitializeOnLoad]` discovery happens during preload; invocation is deduplicated
  per (assembly MVID, swap generation) and stale generations are pruned at swap.
