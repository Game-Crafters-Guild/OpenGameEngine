using System;
using System.IO;
using System.Runtime.InteropServices;

namespace GameEngine.Interop
{
    /// <summary>
    /// Standard result codes returned by native interop calls.
    /// </summary>
    /// <summary>
    /// Result codes returned by native interop calls.
    /// </summary>
    public enum GeResult : int
    {
        /// <summary>Operation completed successfully.</summary>
        Ok = 0,
        /// <summary>Operation failed.</summary>
        Fail = -1
    }

    /// <summary>
    /// Versioned native interface table for scripting interop.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct GE_Interface_v1
    {
        /// <summary>Size of this struct in bytes.</summary>
        public uint sizeBytes;
        /// <summary>Encoded ABI version (major&lt;&lt;16 | minor).</summary>
        public uint abiVersion;
        /// <summary>Pointer to native logging function.</summary>
        public nint Log;
        /// <summary>Pointer to native registration function for managed callbacks.</summary>
        public nint RegisterManagedCallback;
        /// <summary>Pointer to native compilation event notification function.</summary>
        public nint NotifyCompilationEvent;
        /// <summary>Pointer to native reload event notification function.</summary>
        public nint NotifyReloadEvent;
        /// <summary>Pointer to native lookup function for managed callbacks.</summary>
        public nint GetManagedCallback;
        /// <summary>Pointer to native subscribe diagnostics function.</summary>
        public nint SubscribeDiagnostics;
        /// <summary>Pointer to native unsubscribe diagnostics function.</summary>
        public nint UnsubscribeDiagnostics;
        /// <summary>Pointer to native GetAssetCount function.</summary>
        public nint GetAssetCount;
        /// <summary>Pointer to native ScriptsDomainSwap function.</summary>
        public nint ScriptsDomainSwap;
        /// <summary>Pointer to native ScriptsDomainUnload function.</summary>
        public nint ScriptsDomainUnload;
        /// <summary>Pointer to native ECS_GetWorldHandle function.</summary>
        public nint ECS_GetWorldHandle;
        /// <summary>Pointer to native ECS_GetEntityCount function.</summary>
        public nint ECS_GetEntityCount;
    }

    /// <summary>
    /// Managed wrapper for native diagnostics callbacks.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public readonly struct GE_DiagnosticsSink
    {
        /// <summary>Pointer to native callback invoked on compilation events.</summary>
        public readonly nint OnCompilationEvent;
        /// <summary>Pointer to native callback invoked on reload events.</summary>
        public readonly nint OnReloadEvent;
        /// <summary>User data pointer passed back by native when invoking callbacks.</summary>
        public readonly nint UserData;
        /// <summary>Create a diagnostics sink with callback function pointers and opaque user data.</summary>
        public GE_DiagnosticsSink(nint onCompilationEvent, nint onReloadEvent, nint userData)
        {
            OnCompilationEvent = onCompilationEvent;
            OnReloadEvent = onReloadEvent;
            UserData = userData;
        }
    }

    internal static class EngineNativeBootstrap
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate uint GE_ScriptingGetAbiVersion_Delegate();

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int GE_GetInterface_Delegate(uint abiVersion, out nint table, out uint sizeBytes);
    }

    /// <summary>Managed binding to resolve and invoke the native GameEngine ABI.</summary>
    public sealed class EngineNativeBinding : IDisposable
    {
        /// <summary>
        /// Major ABI version; moves WITH the native <c>GE_ABI_VERSION_CURRENT</c> on a breaking change.
        /// 2: the per-project domain exports (<c>GE_ScriptsDomainSwapForProject</c> and friends) were removed.
        /// 3: <c>GE_ScriptingConfig</c> lost <c>projectIdUtf8</c>.
        /// </summary>
        public const uint kAbiMajor = 3;
        /// <summary>
        /// Minor ABI version within <see cref="kAbiMajor"/>. Bump this WITH the native
        /// <c>GE_ABI_VERSION_CURRENT</c> whenever an additive change means managed code reads
        /// something an older native never wrote — most sharply a widened blittable payload
        /// struct, which carries no size field of its own and so cannot be checked per call.
        /// 1: <c>GE_Model_GetExtras</c>, which <c>ModelApi.GetExtras</c> calls.
        /// 2: <c>GE_Animator_PollEventsOnEntity</c>, which <c>AnimatorApi.PollEvents</c> calls.
        /// </summary>
        public const uint kAbiMinor = 2;

        /// <summary>Combined ABI version (major&lt;&lt;16 | minor).</summary>
        public static readonly uint kAbi = (kAbiMajor << 16) | kAbiMinor;

        /// <summary>Library handle returned by NativeLibrary.Load.</summary>
        public nint Handle { get; private set; }

        /// <summary>Resolved interface table from native.</summary>
        public GE_Interface_v1 Iface;


        /// <summary>Native logging delegate signature.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_Log_Delegate(int level, nint msg, uint len);

        /// <summary>Native asset count delegate signature.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_GetAssetCount_Delegate(out int count);

        /// <summary>Resolved logging delegate (if available).</summary>
        public GE_Log_Delegate? Log;

        /// <summary>Resolved GetAssetCount delegate (if available).</summary>
        public GE_GetAssetCount_Delegate? GetAssetCount;

        /// <summary>Delegate to notify native of a scripts domain swap.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_ScriptsDomainSwap_Delegate(ulong newDomain);

        /// <summary>Delegate to get a world handle from native ECS.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_ECS_GetWorldHandle_Delegate(out ulong world);

        /// <summary>Delegate to get entity count from native ECS.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_ECS_GetEntityCount_Delegate(ulong world, out int count);

        /// <summary>Resolved ECS_GetWorldHandle delegate (if available).</summary>
        public GE_ECS_GetWorldHandle_Delegate? ECS_GetWorldHandle;

        /// <summary>Resolved ECS_GetEntityCount delegate (if available).</summary>
        public GE_ECS_GetEntityCount_Delegate? ECS_GetEntityCount;

        /// <summary>Delegate to notify native of scripts domain unload.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_ScriptsDomainUnload_Delegate(ulong domain);


        /// <summary>Resolved ScriptsDomainSwap delegate (if available).</summary>
        public GE_ScriptsDomainSwap_Delegate? ScriptsDomainSwap;

        /// <summary>Resolved ScriptsDomainUnload delegate (if available).</summary>
        public GE_ScriptsDomainUnload_Delegate? ScriptsDomainUnload;


        /// <summary>Delegate to register a managed callback pointer with native.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_RegisterManagedCallback_Delegate(uint id, nint fnPtr);

        /// <summary>Delegate to retrieve a managed callback pointer from native.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_GetManagedCallback_Delegate(uint id, out nint fnPtr);

        /// <summary>Delegate to subscribe diagnostics sink with native.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_SubscribeDiagnostics_Delegate(in GE_DiagnosticsSink sink);
        /// <summary>Delegate to emit compilation events through native diagnostics.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_NotifyCompilationEvent_Delegate(int stage, float progress01, nint diags, uint diagCount);

        /// <summary>Delegate to emit reload events through native diagnostics.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_NotifyReloadEvent_Delegate(int stage, nint reasonUtf8);


        /// <summary>Delegate to unsubscribe diagnostics sink from native.</summary>
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int GE_UnsubscribeDiagnostics_Delegate(in GE_DiagnosticsSink sink);


        /// <summary>Resolved RegisterManagedCallback (if available).</summary>
        public GE_RegisterManagedCallback_Delegate? RegisterManagedCallback;

        /// <summary>Resolved GetManagedCallback (if available).</summary>
        public GE_GetManagedCallback_Delegate? GetManagedCallback;

        /// <summary>Resolved SubscribeDiagnostics (if available).</summary>
        public GE_SubscribeDiagnostics_Delegate? SubscribeDiagnostics;

        /// <summary>Resolved UnsubscribeDiagnostics (if available).</summary>
        public GE_UnsubscribeDiagnostics_Delegate? UnsubscribeDiagnostics;


        /// <summary>
        /// Load native GameEngine library, resolve the interface table and bind delegates.
        /// <paramref name="path"/> is probed before the shared locations (see
        /// NativeEngineLibrary.Load).
        /// </summary>
        public static EngineNativeBinding LoadFrom(string? path, uint? abiOverride = null)
        {
            nint handle = NativeEngineLibrary.Load(path, out string chosen);

            // Record the native directory for native-side bootstrapping when hosted inside non-engine exes.
            // This is consumed by ScriptingABI's EnsureEngineInitialized() to locate Assets/assemblies correctly.
            try
            {
                var dir = Path.GetDirectoryName(chosen);
                if (!string.IsNullOrWhiteSpace(dir))
                    Environment.SetEnvironmentVariable("GE_NATIVE_DIR", dir, EnvironmentVariableTarget.Process);
            }
            catch { }

            if (!NativeLibrary.TryGetExport(handle, "GE_GetInterface", out var getIfacePtr))
                throw new MissingMethodException("GE_GetInterface export not found in " + chosen);


            var getIface = Marshal.GetDelegateForFunctionPointer<EngineNativeBootstrap.GE_GetInterface_Delegate>(getIfacePtr);
            nint tablePtr;
            uint sizeBytes;
            uint abi = abiOverride ?? kAbi;
            int rc = getIface(abi, out tablePtr, out sizeBytes);
            if (rc != 0 || tablePtr == 0) throw new InvalidOperationException($"GE_GetInterface failed rc={rc}");

            var iface = Marshal.PtrToStructure<GE_Interface_v1>(tablePtr);
            int csSize = Marshal.SizeOf<GE_Interface_v1>();
            if (iface.sizeBytes != (uint)csSize)
            {
                throw new InvalidOperationException("ABI struct size mismatch");
            }

            // The MINOR version is not decoration, and native cannot enforce this half.
            //
            // GE_GetInterface compares the major only, which is right for the interface table:
            // it carries sizeBytes, so a table that grew is detectable per call. The blittable
            // payload structs handed to callbacks - GE_UIEventData above all - carry no size,
            // so nothing detects an OLDER native writing a SHORTER struct than this managed
            // build expects to read. That direction is a silent read past the end of a native
            // stack object, and it is exactly what a stale staged GameEngine.Native.dll beside
            // a freshly built managed assembly produces.
            //
            // Fail closed on that one direction. A native that is NEWER than this build is
            // fine: additive fields we do not read cost nothing. Engine and managed ship
            // together from this repo, so a refusal here always means "rebuild", and the
            // message says so rather than leaving a corrupted payload to be debugged later.
            uint nativeMajor = (iface.abiVersion >> 16) & 0xFFFFu;
            uint nativeMinor = iface.abiVersion & 0xFFFFu;
            if (abiOverride == null && nativeMajor == kAbiMajor && nativeMinor < kAbiMinor)
            {
                throw new InvalidOperationException(
                    $"Native engine ABI {nativeMajor}.{nativeMinor} is older than this managed build's "
                    + $"{kAbiMajor}.{kAbiMinor}. Additive fields this build reads from native payload "
                    + $"structs were never written by that native, and those structs carry no size to "
                    + $"check per call. Rebuild the native engine ({chosen}) against the same tree.");
            }

            GE_Log_Delegate? log = iface.Log != 0 ? Marshal.GetDelegateForFunctionPointer<GE_Log_Delegate>(iface.Log) : null;
            GE_GetAssetCount_Delegate? getAssetCount = iface.GetAssetCount != 0 ? Marshal.GetDelegateForFunctionPointer<GE_GetAssetCount_Delegate>(iface.GetAssetCount) : null;
            GE_RegisterManagedCallback_Delegate? regCb = iface.RegisterManagedCallback != 0 ? Marshal.GetDelegateForFunctionPointer<GE_RegisterManagedCallback_Delegate>(iface.RegisterManagedCallback) : null;
            GE_GetManagedCallback_Delegate? getCb = iface.GetManagedCallback != 0 ? Marshal.GetDelegateForFunctionPointer<GE_GetManagedCallback_Delegate>(iface.GetManagedCallback) : null;
            GE_SubscribeDiagnostics_Delegate? subDiag = iface.SubscribeDiagnostics != 0 ? Marshal.GetDelegateForFunctionPointer<GE_SubscribeDiagnostics_Delegate>(iface.SubscribeDiagnostics) : null;
            GE_UnsubscribeDiagnostics_Delegate? unsubDiag = iface.UnsubscribeDiagnostics != 0 ? Marshal.GetDelegateForFunctionPointer<GE_UnsubscribeDiagnostics_Delegate>(iface.UnsubscribeDiagnostics) : null;
            GE_ScriptsDomainSwap_Delegate? domSwap = iface.ScriptsDomainSwap != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ScriptsDomainSwap_Delegate>(iface.ScriptsDomainSwap) : null;
            GE_ECS_GetWorldHandle_Delegate? ecsWorld = iface.ECS_GetWorldHandle != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECS_GetWorldHandle_Delegate>(iface.ECS_GetWorldHandle) : null;
            GE_ECS_GetEntityCount_Delegate? ecsCount = iface.ECS_GetEntityCount != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECS_GetEntityCount_Delegate>(iface.ECS_GetEntityCount) : null;
            GE_ScriptsDomainUnload_Delegate? domUnload = iface.ScriptsDomainUnload != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ScriptsDomainUnload_Delegate>(iface.ScriptsDomainUnload) : null;

            return new EngineNativeBinding {
                Handle = handle,
                Iface = iface,
                Log = log,
                GetAssetCount = getAssetCount,
                RegisterManagedCallback = regCb,
                GetManagedCallback = getCb,
                SubscribeDiagnostics = subDiag,
                UnsubscribeDiagnostics = unsubDiag,
                ScriptsDomainSwap = domSwap,
                ECS_GetWorldHandle = ecsWorld,
                ECS_GetEntityCount = ecsCount,
                ScriptsDomainUnload = domUnload
            };
        }

        /// <summary>Try to get the asset count from native.</summary>
        public int TryGetAssetCount(out int count)
        {
            count = 0;
            return GetAssetCount != null ? GetAssetCount(out count) : -1;
        }

        /// <summary>Try to register a managed callback pointer with native.</summary>
        public int TryRegisterManagedCallback(uint id, nint fnPtr)
        {
            return RegisterManagedCallback != null ? RegisterManagedCallback(id, fnPtr) : -1;
        }

        /// <summary>Try to retrieve a managed callback pointer from native.</summary>
        public int TryGetManagedCallback(uint id, out nint fnPtr)
        {
            fnPtr = 0;
            return GetManagedCallback != null ? GetManagedCallback(id, out fnPtr) : -1;
        }

        /// <summary>Try to get the primary world handle from native.</summary>
        public int TryGetPrimaryWorld(out ulong world)
        {
            world = 0;
            if (ECS_GetWorldHandle == null)
                return (int)GeResult.Fail;
            try
            {
                return ECS_GetWorldHandle(out world);
            }
            catch (System.Runtime.InteropServices.SEHException)
            {
                // Guard against corrupted-state failures bubbling out of native ECS access.
                // Callers should treat any non-zero return code as "world unavailable".
                return (int)GeResult.Fail;
            }
        }

        /// <summary>Try to get the entity count for the given world from native.</summary>
        public int TryGetEntityCount(ulong world, out int count)
        {
            count = 0;
            if (ECS_GetEntityCount == null)
                return (int)GeResult.Fail;
            try
            {
                return ECS_GetEntityCount(world, out count);
            }
            catch (System.Runtime.InteropServices.SEHException)
            {
                // Treat native ECS failures as a simple error code so façade callers can
                // surface a consistent managed InvalidOperationException instead of
                // crashing the test process.
                return (int)GeResult.Fail;
            }
        }

        /// <summary>Try to subscribe a diagnostics sink with native.</summary>
        public int TrySubscribeDiagnostics(in GE_DiagnosticsSink sink)
        {
            return SubscribeDiagnostics != null ? SubscribeDiagnostics(in sink) : -1;
        }

        /// <summary>Try to unsubscribe a diagnostics sink from native.</summary>
        public int TryUnsubscribeDiagnostics(in GE_DiagnosticsSink sink)
        {
            return UnsubscribeDiagnostics != null ? UnsubscribeDiagnostics(in sink) : -1;
        }

        /// <summary>Try to inform native of a scripts domain swap.</summary>
        public int TryScriptsDomainSwap(ulong newDomain) => ScriptsDomainSwap != null ? ScriptsDomainSwap(newDomain) : -1;
        /// <summary>Try to inform native to unload a domain.</summary>
        public int TryScriptsDomainUnload(ulong domain) => ScriptsDomainUnload != null ? ScriptsDomainUnload(domain) : -1;

        /// <summary>Dispose binding resources. Leaves module loaded for process lifetime.</summary>
        public void Dispose()
        {
            if (Handle != 0)
            {
                // Leave the module loaded for process lifetime; unmanaged unload can be unsafe with running code.
                Handle = 0;
            }
        }
    }
}

