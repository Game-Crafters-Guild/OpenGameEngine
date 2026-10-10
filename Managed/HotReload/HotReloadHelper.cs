using System;

namespace GameEngine.HotReload
{
    /// <summary>
    /// Convenience helpers for common hot-reload flows.
    /// Keeps interop-facing API simple while centralizing common validation.
    /// </summary>
    public static class HotReloadHelper
    {
        // CoreBridge ScriptingOpResult.InvalidArg (GE_Result_InvalidArg); this assembly does not reference CoreBridge.
        private const int kInvalidArg = -2;

        /// <summary>
        /// Preloads an assembly context from compiled bytes and performs an atomic swap.
        /// Returns ScriptingOpResult.Ok (0) on success; on failure returns a negative ScriptingOpResult code.
        /// </summary>
        public static int SwapFromBytes(byte[] assemblyBytes, byte[]? pdbBytes = null)
        {
            if (assemblyBytes == null || assemblyBytes.Length == 0)
                return kInvalidArg;

            int rc = HotReloadManager.PreloadAssemblyContext(assemblyBytes, pdbBytes);
            if (rc != 0) return rc;
            rc = HotReloadManager.SwapPreloadedContext();
            return rc;
        }

        /// <summary>
        /// Loads compiled assembly bytes into a new collectible context and returns the managed domain id (>0),
        /// or a negative ScriptingOpResult code on failure.
        /// </summary>
        public static int LoadFromBytes(byte[] assemblyBytes)
        {
            if (assemblyBytes == null || assemblyBytes.Length == 0)
                return kInvalidArg;
            return HotReloadManager.LoadCompiledAssembly(assemblyBytes);
        }

        /// <summary>
        /// Facade: Query a public static int (no args) export by name.
        /// Returns ScriptingOpResult.Ok (0) and sets 'token' on success; negative code on failure.
        /// </summary>
        public static int TryQuery(string methodName, out ulong token, ulong domain = 0)
        {
            token = 0UL;
            return HotReloadManager.QueryExportInDomain(domain, methodName, ref token);
        }

        /// <summary>
        /// Facade: Invoke a previously queried token. Returns the method's int result on success; negative ScriptingOpResult on failure.
        /// </summary>
        public static int TryInvoke(ulong token, ulong domain = 0)
        {
            return HotReloadManager.InvokeByToken(domain, token);
        }
    }
}
