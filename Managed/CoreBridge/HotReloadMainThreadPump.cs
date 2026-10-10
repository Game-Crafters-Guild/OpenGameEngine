using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace GameEngine.CoreBridge;

/// <summary>
/// Native-callable bridge for HotReloadManager's engine main-thread marshaling (B5):
/// the host registers a pump-request callback, HotReloadManager queues managed work
/// ([InitializeOnLoad]) instead of running it on a thread-pool thread concurrent with
/// rendering, and the host's per-frame main-thread task pump calls back into
/// <see cref="PumpHotReloadMainThread"/> to drain it.
/// </summary>
public static unsafe partial class CoreBridge
{
    private static MethodInfo? s_miSetMainThreadPump;
    private static MethodInfo? s_miDrainMainThreadWork;

    /// <summary>
    /// Registers a native pump-request callback (void(GE_CDECL*)(void* user)) with
    /// HotReloadManager. Pass fn == 0 to clear. Returns 0 on success.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int SetMainThreadPumpCallback(IntPtr fn, IntPtr user)
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) return -3;
            s_miSetMainThreadPump ??= s_hotReloadType!.GetMethod(
                "SetMainThreadPump", BindingFlags.Public | BindingFlags.Static);
            if (s_miSetMainThreadPump == null) return -3;

            Action? requestPump = null;
            if (fn != IntPtr.Zero)
            {
                IntPtr fnCopy = fn;
                IntPtr userCopy = user;
                requestPump = () => ((delegate* unmanaged[Cdecl]<void*, void>)fnCopy)((void*)userCopy);
            }
            var ret = s_miSetMainThreadPump.Invoke(null, new object?[] { requestPump });
            return ret is int i ? i : 0;
        }
        catch { return -1; }
    }

    /// <summary>
    /// Drains HotReloadManager's queued main-thread work. Call from the engine main
    /// thread. Returns the number of items run, or negative on failure.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int PumpHotReloadMainThread()
    {
        try
        {
            if (!EnsureHotReloadTypeLoaded()) return -3;
            s_miDrainMainThreadWork ??= s_hotReloadType!.GetMethod(
                "DrainMainThreadWork", BindingFlags.Public | BindingFlags.Static);
            if (s_miDrainMainThreadWork == null) return -3;
            var ret = s_miDrainMainThreadWork.Invoke(null, null);
            return ret is int i ? i : 0;
        }
        catch { return -1; }
    }
}
