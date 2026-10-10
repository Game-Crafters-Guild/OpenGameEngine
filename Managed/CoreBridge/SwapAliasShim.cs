using System;

namespace GameEngine.CoreBridge
{
    internal static class SwapAliasShim
    {
        // Resolves HotReloadManager.SwapPreloadedContext() when the generated binder
        // has not cached it yet.
        internal static System.Reflection.MethodInfo? ResolveSwapAlias(Type hrmType)
        {
            return hrmType.GetMethod("SwapPreloadedContext", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
        }
    }
}

