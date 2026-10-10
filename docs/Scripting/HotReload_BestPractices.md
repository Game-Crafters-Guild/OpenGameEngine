# Hot Reload and Scripting Best Practices

This guide summarizes the recommended usage patterns for GameEngine hot reload and the IncrementalCompiler to maximize iteration speed and stability.

## Public export shape for fast invocation
- Exports are public static int methods with no parameters.
- Keep names stable. Prefer fully qualified names (Namespace.Type.Method) when querying.
- Use HotReloadHelper facades:
  - TryQuery(string name, out ulong token, ulong domain = 0)
  - TryInvoke(ulong token, ulong domain = 0)
- Return ScriptingOpResult.Ok (0) on success; use negative codes from ScriptingOpResult for errors.

## Token lifecycle and domains
- Tokens are stable within a domain for a given export; they may become invalid after swaps.
- After SwapPreloadedContext or LoadCompiledAssembly, re-query tokens.
- Domain 0 means "current domain". HotReloadManager maintains a positive domain id per loaded context.

## Query/Invoke performance
- QueryExportInDomain uses a prebuilt index (full name and simple name) for O(1) lookup after load/swap.
- Try to query by full name when possible; simple names are supported but first-match wins.
- Cache tokens on the caller side; invoke by token on the hot path.

## Assembly loading and swapping
- For fastest swaps:
  1) PreloadAssemblyContext(byte[] asm, byte[]? pdb)
  2) SwapPreloadedContext()
- Preloading does all heavy work (context creation, load) off the main thread; swap is atomic and very fast.
- Loading by file path is supported, but stream/bytes loading avoids file locks.

## IncrementalCompiler tips
- Prefer CompileIncremental for small changes; falls back to CompileFull when needed.
- The compiler caches:
  - SyntaxTrees per file
  - Quick fingerprints (length + last write time) to skip unchanged files
  - Metadata references with a ReferencesKey fingerprint
  - Fast result cache (bounded + TTL) for no-op iterations
- No-op runs (no effective changes) are served from fast cache or re-emitted from the base compilation.
- If you see frequent full rebuilds, check that ProjectPath remains stable and changes are small in count.

## Environment flags
- GE_HOTRELOAD_VERBOSE=1 enables info-level logs for HRM and IncrementalCompiler.
  - Default is quiet; warnings/errors still print.
- GE_HRM_PATH can be used by tests/tools to locate the built HRM assembly.

## Error handling
- Methods return int codes; 0 is success. CoreBridge maps them to its ScriptingOpResult, whose values are GE_Result's.
- Common failure codes include Fail(-1), InvalidArg(-2), NotFound(-3) and NotInitialized(-4); a missing script assembly file is NotFound.

## Testing patterns
- Use HotReloadHelper in tests to reduce reflection and boilerplate.
- Micro-benchmarks are marked [Explicit] to avoid running in CI:
  - Query/Invoke warm paths
  - IncrementalCompiler no-change and small-change loops

## When to clear caches
- For rare pathologies (runtime update, drastic project changes), ClearCache on IncrementalCompiler:
  - Clears syntax trees, references, base compilation, fast cache entries, and reference fingerprints.

## Diagnostics
- HRM keeps minimal logs by default for clean test output.
- Enable verbose mode during investigation; leave it off for normal runs to avoid overhead.

