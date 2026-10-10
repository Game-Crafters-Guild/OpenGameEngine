using System;
using System.Runtime.InteropServices;
using GameEngine.Interop;

namespace GameEngine.Physics
{
    public static class Physics
    {
        public static WorldHandle DefaultWorld
        {
            get
            {
                ulong h = Native.GetDefaultWorld();
                return new WorldHandle(h);
            }
        }
    }

    public readonly struct WorldHandle
    {
        private readonly ulong m_handle;
        internal WorldHandle(ulong handle) { m_handle = handle; }
        public ulong Handle => m_handle;

        public void Step(float deltaTime, int collisionSteps = 1)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            int rc = Native.TryStep(m_handle, deltaTime, collisionSteps);
            if (rc != 0) throw new InvalidOperationException($"Physics Step failed rc={rc}");
        }
    }

    internal static unsafe class Native
    {
        private const uint kAbiMajor = 1;
        private const uint kAbiMinor = 0;
        private static readonly uint kAbi = (kAbiMajor << 16) | kAbiMinor;

        [StructLayout(LayoutKind.Sequential)]
        private struct GE_Physics_Interface_v1
        {
            public uint sizeBytes;
            public uint abiVersion;
            public nint GetDefaultWorld;
            public nint StepWorld;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_Physics_GetInterface_Delegate(uint abiVersion, out nint table, out uint sizeBytes);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_PhysicsABI_GetDefaultWorld_Delegate(out ulong world);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_PhysicsABI_StepWorld_Delegate(ulong world, float deltaTime, int collisionSteps);

        private static readonly object s_lock = new object();
        private static bool s_loaded;
        private static nint s_nativeHandle;

        private static GE_PhysicsABI_GetDefaultWorld_Delegate? s_getDefaultWorld;
        private static GE_PhysicsABI_StepWorld_Delegate? s_stepWorld;

        private static void EnsureLoaded()
        {
            if (s_loaded) return;
            lock (s_lock)
            {
                if (s_loaded) return;

                s_nativeHandle = NativeEngineLibrary.Load(null, out _);

                if (!NativeLibrary.TryGetExport(s_nativeHandle, "GE_Physics_GetInterface", out nint getIfacePtr) || getIfacePtr == 0)
                    throw new MissingMethodException("GE_Physics_GetInterface export not found in GameEngine.Native");

                var getIface = Marshal.GetDelegateForFunctionPointer<GE_Physics_GetInterface_Delegate>(getIfacePtr);
                int rc = getIface(kAbi, out nint tablePtr, out uint sizeBytes);
                if (rc != 0 || tablePtr == 0 || sizeBytes == 0)
                    throw new InvalidOperationException($"GE_Physics_GetInterface failed rc={rc}");

                var iface = Marshal.PtrToStructure<GE_Physics_Interface_v1>(tablePtr);
                int csSize = Marshal.SizeOf<GE_Physics_Interface_v1>();
                if (iface.sizeBytes != (uint)csSize)
                    throw new InvalidOperationException("Physics ABI struct size mismatch");

                s_getDefaultWorld = iface.GetDefaultWorld != 0 ? Marshal.GetDelegateForFunctionPointer<GE_PhysicsABI_GetDefaultWorld_Delegate>(iface.GetDefaultWorld) : null;
                s_stepWorld = iface.StepWorld != 0 ? Marshal.GetDelegateForFunctionPointer<GE_PhysicsABI_StepWorld_Delegate>(iface.StepWorld) : null;

                s_loaded = true;
            }
        }

        public static ulong GetDefaultWorld()
        {
            EnsureLoaded();
            if (s_getDefaultWorld == null)
                throw new MissingMethodException("Physics ABI GetDefaultWorld not available");
            int rc = s_getDefaultWorld(out ulong world);
            if (rc != 0 || world == 0)
                throw new InvalidOperationException($"Default physics world not available rc={rc}");
            return world;
        }

        public static int TryStep(ulong world, float deltaTime, int collisionSteps)
        {
            EnsureLoaded();
            if (s_stepWorld == null)
                return -1;
            return s_stepWorld(world, deltaTime, collisionSteps);
        }
    }
}

