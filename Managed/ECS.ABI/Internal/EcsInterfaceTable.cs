using System;
using System.Runtime.InteropServices;
using GameEngine.Interop;

namespace GameEngine.ECS.Internal
{
    /// <summary>
    /// Mirror of the native GE_ECS_Interface_v2 struct layout (ECSABI.h). Field offsets, not
    /// names, are the contract: the order must match ECSABI.h exactly, and new native fields
    /// are appended at the end.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct GE_ECS_Interface_v2
    {
        public uint sizeBytes;
        public uint abiVersion;

        // v1.0-1.2 fields (15 function pointers; legacy QueryCreate/QueryDestroy/
        // QueryNext were deleted in v2.0 — CachedQuery is the sole query surface)
        public nint GetPrimaryWorld;
        public nint GetEntityCount;
        public nint CreateEmptyEntity;
        public nint CreatePrimitive;
        public nint IsEntityValid;
        public nint DestroyEntity;
        public nint SetName;
        public nint GetName;
        public nint GetTransform;
        public nint SetTransform;
        public nint RegisterBlobComponent;
        public nint GetComponentTypeIdByName;
        public nint SetComponentBytes;
        public nint GetComponentBytes;
        public nint RemoveComponent;

        // v1.3 fields (9 function pointers)
        public nint CachedQueryCreate;
        public nint CachedQueryDestroy;
        public nint CachedQueryReset;
        public nint CachedQueryGetArchetypeInfo;
        public nint CachedQueryGetChunkData;
        public nint CachedQueryGetChunkEntityIds;
        public nint HasComponent;
        public nint SetDeferStructuralChanges;
        public nint FlushDeferredCommands;

        // v1.4 fields (1 function pointer)
        public nint CachedQueryArchetypeHasComponent;

        // v1.5 fields (2 function pointers)
        public nint CreateEntityRaw;
        public nint DeferCommand;

        // v1.6 fields (2 function pointers)
        public nint CachedQueryGetComponentSlice;
        public nint CachedQueryGetEntityIdSlice;

        // v1.7 fields (2 function pointers): change-signaling chunk
        // enumeration (chunkVersion out param) + read-only span entry
        public nint CachedQueryGetChunkDataV2;
        public nint CachedQueryGetChunkDataReadOnly;

        // v2.1 fields (1 function pointer): blob component registration with a
        // reflected field table
        public nint RegisterBlobComponentWithSchema;

        // v2.2 fields (1 function pointer): the tag that switches a component off
        public nint GetComponentDisabledTypeId;

        // v2.3 fields (1 function pointer): the entity switch (World::SetEntityEnabledImmediate)
        public nint SetEntityEnabled;
    }

    /// <summary>
    /// Loads GameEngine.Native and fetches the ECS ABI interface table through
    /// GE_ECS_GetInterface. The one place this assembly states the ABI version it binds.
    /// </summary>
    internal static class EcsInterfaceTable
    {
        private const uint kAbiMajor = 2;
        private const uint kAbiMinor = 3;
        private static readonly uint kAbi = (kAbiMajor << 16) | kAbiMinor;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECS_GetInterface_Delegate(uint abiVersion, out nint table, out uint sizeBytes);

        /// <summary>
        /// Returns a copy of the native interface table. Throws when the native engine cannot
        /// be loaded, lacks the export, rejects the ABI version, or is older than this mirror.
        /// </summary>
        internal static GE_ECS_Interface_v2 Acquire()
        {
            nint nativeHandle = NativeEngineLibrary.Load(null, out _);

            if (!NativeLibrary.TryGetExport(nativeHandle, "GE_ECS_GetInterface", out nint getIfacePtr) || getIfacePtr == 0)
                throw new MissingMethodException("GE_ECS_GetInterface export not found in GameEngine.Native");

            var getIface = Marshal.GetDelegateForFunctionPointer<GE_ECS_GetInterface_Delegate>(getIfacePtr);
            int rc = getIface(kAbi, out nint tablePtr, out uint sizeBytes);
            if (rc != 0 || tablePtr == 0 || sizeBytes == 0)
                throw new InvalidOperationException($"GE_ECS_GetInterface failed rc={rc}");

            var iface = Marshal.PtrToStructure<GE_ECS_Interface_v2>(tablePtr);
            int csSize = Marshal.SizeOf<GE_ECS_Interface_v2>();
            // A larger native table is fine: fields are only ever appended.
            if (iface.sizeBytes < (uint)csSize)
                throw new InvalidOperationException(
                    $"ECS ABI struct size mismatch (native={iface.sizeBytes}, managed={csSize}). " +
                    "Native engine is older than the managed ECS.ABI assembly.");
            return iface;
        }
    }
}
