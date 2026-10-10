using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace GameEngine.ECS.Internal
{
    /// <summary>
    /// P/Invoke layer for the ECS ABI cached chunk query functions.
    /// Used by source-generated IEntitySystem code. Not intended for direct use —
    /// the GameEngine.ECS.Internal namespace signals this is engine plumbing.
    /// Public because generated code in user assemblies must access it.
    /// </summary>
    public static unsafe class ChunkQueryNative
    {
        // Delegate types for v1.3 functions
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryCreate_Delegate(
            ulong world, ulong* requiredTypeIds, uint requiredCount,
            ulong* excludedTypeIds, uint excludedCount, out ulong query);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryDestroy_Delegate(ulong query);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryReset_Delegate(ulong query, out uint archetypeCount);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryGetArchetypeInfo_Delegate(
            ulong query, uint archetypeIndex, out uint entityCount, out uint chunkCount);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryGetChunkEntityIds_Delegate(
            ulong query, uint archetypeIndex, uint chunkIndex,
            out uint* outEntityIds, out uint outCount);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int HasComponent_Delegate(
            ulong world, uint entity, ulong componentTypeId, out int outHas);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SetDeferStructuralChanges_Delegate(ulong world, int defer);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int FlushDeferredCommands_Delegate(ulong world);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryArchetypeHasComponent_Delegate(
            ulong query, uint archetypeIndex, ulong componentTypeId, out int outHas);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CreateEntityRaw_Delegate(ulong world, out uint entity);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int DeferCommand_Delegate(
            ulong world, uint commandType, uint entity, ulong componentTypeId,
            void* data, uint dataLen);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SetEntityEnabled_Delegate(ulong world, uint entity, int enabled);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryGetComponentSlice_Delegate(
            ulong query, uint archetypeIndex, ulong componentTypeId,
            uint entityOffset, out void* outData, out uint outCount, out uint outStride);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryGetEntityIdSlice_Delegate(
            ulong query, uint archetypeIndex, uint entityOffset,
            uint maxCount, out uint* outEntityIds, out uint outCount);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryGetChunkDataV2_Delegate(
            ulong query, uint archetypeIndex, uint chunkIndex, ulong componentTypeId,
            out void* outData, out uint outCount, out uint outStride, out ulong outChunkVersion);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CachedQueryGetChunkDataReadOnly_Delegate(
            ulong query, uint archetypeIndex, uint chunkIndex, ulong componentTypeId,
            out void* outData, out uint outCount, out uint outStride, out ulong outChunkVersion);

        // Cached delegate instances
        private static readonly object s_lock = new object();
        private static volatile bool s_loaded;

        private static CachedQueryCreate_Delegate? s_cachedQueryCreate;
        private static CachedQueryDestroy_Delegate? s_cachedQueryDestroy;
        private static CachedQueryReset_Delegate? s_cachedQueryReset;
        private static CachedQueryGetArchetypeInfo_Delegate? s_cachedQueryGetArchetypeInfo;
        private static CachedQueryGetChunkEntityIds_Delegate? s_cachedQueryGetChunkEntityIds;
        private static HasComponent_Delegate? s_hasComponent;
        private static SetDeferStructuralChanges_Delegate? s_setDeferStructuralChanges;
        private static FlushDeferredCommands_Delegate? s_flushDeferredCommands;
        private static CachedQueryArchetypeHasComponent_Delegate? s_cachedQueryArchetypeHasComponent;
        private static CreateEntityRaw_Delegate? s_createEntityRaw;
        private static DeferCommand_Delegate? s_deferCommand;
        private static SetEntityEnabled_Delegate? s_setEntityEnabled;
        private static CachedQueryGetComponentSlice_Delegate? s_cachedQueryGetComponentSlice;
        private static CachedQueryGetEntityIdSlice_Delegate? s_cachedQueryGetEntityIdSlice;
        private static CachedQueryGetChunkDataV2_Delegate? s_cachedQueryGetChunkDataV2;
        private static CachedQueryGetChunkDataReadOnly_Delegate? s_cachedQueryGetChunkDataReadOnly;

        private static void EnsureLoaded()
        {
            if (s_loaded)
                return;

            lock (s_lock)
            {
                if (s_loaded)
                    return;

                var iface = EcsInterfaceTable.Acquire();

                s_cachedQueryCreate = iface.CachedQueryCreate != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryCreate_Delegate>(iface.CachedQueryCreate) : null;
                s_cachedQueryDestroy = iface.CachedQueryDestroy != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryDestroy_Delegate>(iface.CachedQueryDestroy) : null;
                s_cachedQueryReset = iface.CachedQueryReset != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryReset_Delegate>(iface.CachedQueryReset) : null;
                s_cachedQueryGetArchetypeInfo = iface.CachedQueryGetArchetypeInfo != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryGetArchetypeInfo_Delegate>(iface.CachedQueryGetArchetypeInfo) : null;
                s_cachedQueryGetChunkEntityIds = iface.CachedQueryGetChunkEntityIds != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryGetChunkEntityIds_Delegate>(iface.CachedQueryGetChunkEntityIds) : null;
                s_hasComponent = iface.HasComponent != 0
                    ? Marshal.GetDelegateForFunctionPointer<HasComponent_Delegate>(iface.HasComponent) : null;
                s_setDeferStructuralChanges = iface.SetDeferStructuralChanges != 0
                    ? Marshal.GetDelegateForFunctionPointer<SetDeferStructuralChanges_Delegate>(iface.SetDeferStructuralChanges) : null;
                s_flushDeferredCommands = iface.FlushDeferredCommands != 0
                    ? Marshal.GetDelegateForFunctionPointer<FlushDeferredCommands_Delegate>(iface.FlushDeferredCommands) : null;
                s_cachedQueryArchetypeHasComponent = iface.CachedQueryArchetypeHasComponent != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryArchetypeHasComponent_Delegate>(iface.CachedQueryArchetypeHasComponent) : null;
                s_createEntityRaw = iface.CreateEntityRaw != 0
                    ? Marshal.GetDelegateForFunctionPointer<CreateEntityRaw_Delegate>(iface.CreateEntityRaw) : null;
                s_deferCommand = iface.DeferCommand != 0
                    ? Marshal.GetDelegateForFunctionPointer<DeferCommand_Delegate>(iface.DeferCommand) : null;
                s_setEntityEnabled = iface.SetEntityEnabled != 0
                    ? Marshal.GetDelegateForFunctionPointer<SetEntityEnabled_Delegate>(iface.SetEntityEnabled) : null;
                s_cachedQueryGetComponentSlice = iface.CachedQueryGetComponentSlice != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryGetComponentSlice_Delegate>(iface.CachedQueryGetComponentSlice) : null;
                s_cachedQueryGetEntityIdSlice = iface.CachedQueryGetEntityIdSlice != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryGetEntityIdSlice_Delegate>(iface.CachedQueryGetEntityIdSlice) : null;
                s_cachedQueryGetChunkDataV2 = iface.CachedQueryGetChunkDataV2 != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryGetChunkDataV2_Delegate>(iface.CachedQueryGetChunkDataV2) : null;
                s_cachedQueryGetChunkDataReadOnly = iface.CachedQueryGetChunkDataReadOnly != 0
                    ? Marshal.GetDelegateForFunctionPointer<CachedQueryGetChunkDataReadOnly_Delegate>(iface.CachedQueryGetChunkDataReadOnly) : null;

                s_loaded = true;
            }
        }

        // ---- Public API (called by source-generated code) ----

        /// <summary>
        /// Creates a cached query for chunk-based iteration.
        /// Returns an opaque query handle (must be destroyed via DestroyCachedQuery).
        /// </summary>
        public static ulong CreateCachedQuery(ulong worldHandle,
            ReadOnlySpan<ulong> required, ReadOnlySpan<ulong> excluded)
        {
            EnsureLoaded();
            if (s_cachedQueryCreate == null)
                throw new MissingMethodException("ECS ABI CachedQueryCreate not available");

            fixed (ulong* req = required)
            fixed (ulong* exc = excluded)
            {
                int rc = s_cachedQueryCreate(worldHandle,
                    required.Length == 0 ? null : req, (uint)required.Length,
                    excluded.Length == 0 ? null : exc, (uint)excluded.Length,
                    out ulong query);
                if (rc != 0 || query == 0)
                    throw new InvalidOperationException($"CachedQueryCreate failed rc={rc}");
                return query;
            }
        }

        /// <summary>
        /// Destroys a cached query handle.
        /// </summary>
        public static void DestroyCachedQuery(ulong queryHandle)
        {
            EnsureLoaded();
            if (s_cachedQueryDestroy == null)
                throw new MissingMethodException("ECS ABI CachedQueryDestroy not available");

            int rc = s_cachedQueryDestroy(queryHandle);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryDestroy failed rc={rc}");
        }

        /// <summary>
        /// Resets iteration state and refreshes cached archetypes if the world has changed.
        /// Returns the number of matching archetypes.
        /// </summary>
        public static void ResetQuery(ulong queryHandle, out int archetypeCount)
        {
            EnsureLoaded();
            if (s_cachedQueryReset == null)
                throw new MissingMethodException("ECS ABI CachedQueryReset not available");

            int rc = s_cachedQueryReset(queryHandle, out uint count);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryReset failed rc={rc}");
            archetypeCount = (int)count;
        }

        /// <summary>
        /// Gets entity count and chunk count for a specific archetype in the query results.
        /// </summary>
        public static void GetArchetypeInfo(ulong queryHandle, int archetypeIndex,
            out int entityCount, out int chunkCount)
        {
            EnsureLoaded();
            if (s_cachedQueryGetArchetypeInfo == null)
                throw new MissingMethodException("ECS ABI CachedQueryGetArchetypeInfo not available");

            int rc = s_cachedQueryGetArchetypeInfo(queryHandle, (uint)archetypeIndex,
                out uint entities, out uint chunks);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryGetArchetypeInfo failed rc={rc}");
            entityCount = (int)entities;
            chunkCount = (int)chunks;
        }

        /// <summary>
        /// Returns a writable Span backed by native chunk memory for the given component.
        /// The span is valid until the next structural change or query reset.
        /// Records a change-signaling write grant on the chunk's column (v1.7);
        /// use GetChunkReadOnlySpan for reads so they don't over-stamp.
        /// </summary>
        public static Span<T> GetChunkSpan<T>(ulong queryHandle,
            int archetypeIndex, int chunkIndex, ulong componentTypeId) where T : unmanaged
        {
            EnsureLoaded();
            if (s_cachedQueryGetChunkDataV2 == null)
                throw new MissingMethodException("ECS ABI CachedQueryGetChunkDataV2 not available");

            int rc = s_cachedQueryGetChunkDataV2(queryHandle,
                (uint)archetypeIndex, (uint)chunkIndex, componentTypeId,
                out void* data, out uint count, out uint stride, out _);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryGetChunkDataV2 failed rc={rc}");

            if (stride != (uint)Unsafe.SizeOf<T>())
                throw new InvalidOperationException(
                    $"Component stride mismatch: native={stride}, managed={Unsafe.SizeOf<T>()}. " +
                    "The C# struct layout does not match the registered component size.");

            if (data == null || count == 0)
                return Span<T>.Empty;

            return new Span<T>(data, (int)count);
        }

        /// <summary>
        /// Returns a read-only Span backed by native chunk memory for the given component.
        /// The span is valid until the next structural change or query reset.
        /// Uses the v1.7 read-only entry so managed reads never record a
        /// change-signaling write grant (no over-stamping).
        /// </summary>
        public static ReadOnlySpan<T> GetChunkReadOnlySpan<T>(ulong queryHandle,
            int archetypeIndex, int chunkIndex, ulong componentTypeId) where T : unmanaged
        {
            EnsureLoaded();
            if (s_cachedQueryGetChunkDataReadOnly == null)
                throw new MissingMethodException("ECS ABI CachedQueryGetChunkDataReadOnly not available");

            int rc = s_cachedQueryGetChunkDataReadOnly(queryHandle,
                (uint)archetypeIndex, (uint)chunkIndex, componentTypeId,
                out void* data, out uint count, out uint stride, out _);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryGetChunkDataReadOnly failed rc={rc}");

            if (stride != (uint)Unsafe.SizeOf<T>())
                throw new InvalidOperationException(
                    $"Component stride mismatch: native={stride}, managed={Unsafe.SizeOf<T>()}. " +
                    "The C# struct layout does not match the registered component size.");

            if (data == null || count == 0)
                return ReadOnlySpan<T>.Empty;

            return new ReadOnlySpan<T>(data, (int)count);
        }

        /// <summary>
        /// Returns entity IDs for all entities in a chunk.
        /// The span is valid until the next structural change or query reset.
        /// </summary>
        public static ReadOnlySpan<uint> GetChunkEntityIds(ulong queryHandle,
            int archetypeIndex, int chunkIndex)
        {
            EnsureLoaded();
            if (s_cachedQueryGetChunkEntityIds == null)
                throw new MissingMethodException("ECS ABI CachedQueryGetChunkEntityIds not available");

            int rc = s_cachedQueryGetChunkEntityIds(queryHandle,
                (uint)archetypeIndex, (uint)chunkIndex,
                out uint* entityIds, out uint count);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryGetChunkEntityIds failed rc={rc}");

            if (entityIds == null || count == 0)
                return ReadOnlySpan<uint>.Empty;

            return new ReadOnlySpan<uint>(entityIds, (int)count);
        }

        /// <summary>
        /// Checks whether the given entity has a component of the specified type.
        /// </summary>
        public static bool HasComponent(ulong worldHandle, uint entityId, ulong componentTypeId)
        {
            EnsureLoaded();
            if (s_hasComponent == null)
                throw new MissingMethodException("ECS ABI HasComponent not available");

            int rc = s_hasComponent(worldHandle, entityId, componentTypeId, out int has);
            if (rc != 0)
                throw new InvalidOperationException($"HasComponent failed rc={rc}");
            return has != 0;
        }

        /// <summary>
        /// Enables or disables deferred structural changes on the world.
        /// While deferred, entity create/destroy and component add/remove are queued.
        /// </summary>
        public static void SetDeferStructuralChanges(ulong worldHandle, bool defer)
        {
            EnsureLoaded();
            if (s_setDeferStructuralChanges == null)
                throw new MissingMethodException("ECS ABI SetDeferStructuralChanges not available");

            int rc = s_setDeferStructuralChanges(worldHandle, defer ? 1 : 0);
            if (rc != 0)
                throw new InvalidOperationException($"SetDeferStructuralChanges failed rc={rc}");
        }

        /// <summary>
        /// Checks whether the archetype at the given index in a cached query contains
        /// the specified component type. Used by generated code for optional component support.
        /// </summary>
        public static bool ArchetypeHasComponent(ulong queryHandle, int archetypeIndex, ulong componentTypeId)
        {
            EnsureLoaded();
            if (s_cachedQueryArchetypeHasComponent == null)
                throw new MissingMethodException("ECS ABI CachedQueryArchetypeHasComponent not available");

            int rc = s_cachedQueryArchetypeHasComponent(queryHandle, (uint)archetypeIndex, componentTypeId, out int has);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryArchetypeHasComponent failed rc={rc}");
            return has != 0;
        }

        /// <summary>
        /// Flushes all deferred structural commands (entity create/destroy, component add/remove).
        /// Typically called at the end of the managed system tick.
        /// </summary>
        public static void FlushDeferredCommands(ulong worldHandle)
        {
            EnsureLoaded();
            if (s_flushDeferredCommands == null)
                throw new MissingMethodException("ECS ABI FlushDeferredCommands not available");

            int rc = s_flushDeferredCommands(worldHandle);
            if (rc != 0)
                throw new InvalidOperationException($"FlushDeferredCommands failed rc={rc}");
        }

        /// <summary>
        /// Creates a new empty entity (no components). Returns a valid entity ID immediately.
        /// Used by EntityCommands for deferred structural operations.
        /// </summary>
        public static uint CreateEntityRaw(ulong worldHandle)
        {
            EnsureLoaded();
            if (s_createEntityRaw == null)
                throw new MissingMethodException("ECS ABI CreateEntityRaw not available");

            int rc = s_createEntityRaw(worldHandle, out uint entity);
            if (rc != 0)
                throw new InvalidOperationException($"CreateEntityRaw failed rc={rc}");
            return entity;
        }

        /// <summary>
        /// Queues a deferred structural command (destroy, add/remove/set component).
        /// Command types: 1=Destroy, 2=AddComponent, 3=RemoveComponent, 4=SetComponent.
        /// </summary>
        public static void DeferCommand(ulong worldHandle, uint commandType,
            uint entity, ulong componentTypeId, void* data, uint dataLen)
        {
            EnsureLoaded();
            if (s_deferCommand == null)
                throw new MissingMethodException("ECS ABI DeferCommand not available");

            int rc = s_deferCommand(worldHandle, commandType, entity, componentTypeId, data, dataLen);
            if (rc != 0)
                throw new InvalidOperationException($"DeferCommand failed rc={rc}");
        }

        /// <summary>
        /// Switches an entity on or off: ECS.Disabled and the derived DisabledInHierarchy move
        /// together, so an entity switched back on returns to queries at once, unless its parent
        /// is still off, which keeps the derived tag until the hierarchy pass. Queued while
        /// structural changes are deferred (applied at the flush), immediate otherwise.
        /// </summary>
        public static void SetEntityEnabled(ulong worldHandle, uint entity, bool enabled)
        {
            EnsureLoaded();
            if (s_setEntityEnabled == null)
                throw new MissingMethodException("ECS ABI SetEntityEnabled not available");

            int rc = s_setEntityEnabled(worldHandle, entity, enabled ? 1 : 0);
            if (rc != 0)
                throw new InvalidOperationException($"SetEntityEnabled failed rc={rc}");
        }

        // ---- v1.6: Slice-based iteration ----

        /// <summary>
        /// Returns a writable Span for a contiguous slice of component data starting at
        /// entityOffset within the archetype. The returned count may be less than the
        /// remaining entities if a chunk boundary is hit. Handles both BlobComponentArray
        /// (single chunk, always returns all remaining) and ComponentArray&lt;T&gt; (multi-chunk).
        /// </summary>
        public static Span<T> GetComponentSlice<T>(ulong queryHandle,
            int archetypeIndex, ulong componentTypeId, int entityOffset)
            where T : unmanaged
        {
            EnsureLoaded();
            if (s_cachedQueryGetComponentSlice == null)
                throw new MissingMethodException("ECS ABI CachedQueryGetComponentSlice not available (v1.6 required)");

            int rc = s_cachedQueryGetComponentSlice(queryHandle,
                (uint)archetypeIndex, componentTypeId, (uint)entityOffset,
                out void* data, out uint count, out uint stride);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryGetComponentSlice failed rc={rc}");

            if (stride != (uint)Unsafe.SizeOf<T>())
                throw new InvalidOperationException(
                    $"Component stride mismatch: native={stride}, managed={Unsafe.SizeOf<T>()}. " +
                    "The C# struct layout does not match the registered component size.");

            if (data == null || count == 0)
                return Span<T>.Empty;

            return new Span<T>(data, (int)count);
        }

        /// <summary>
        /// Returns a read-only Span for a contiguous slice of component data starting at
        /// entityOffset within the archetype.
        /// </summary>
        public static ReadOnlySpan<T> GetComponentSliceReadOnly<T>(ulong queryHandle,
            int archetypeIndex, ulong componentTypeId, int entityOffset)
            where T : unmanaged
        {
            EnsureLoaded();
            if (s_cachedQueryGetComponentSlice == null)
                throw new MissingMethodException("ECS ABI CachedQueryGetComponentSlice not available (v1.6 required)");

            int rc = s_cachedQueryGetComponentSlice(queryHandle,
                (uint)archetypeIndex, componentTypeId, (uint)entityOffset,
                out void* data, out uint count, out uint stride);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryGetComponentSlice failed rc={rc}");

            if (stride != (uint)Unsafe.SizeOf<T>())
                throw new InvalidOperationException(
                    $"Component stride mismatch: native={stride}, managed={Unsafe.SizeOf<T>()}. " +
                    "The C# struct layout does not match the registered component size.");

            if (data == null || count == 0)
                return ReadOnlySpan<T>.Empty;

            return new ReadOnlySpan<T>(data, (int)count);
        }

        /// <summary>
        /// Returns entity IDs for a slice of entities starting at entityOffset.
        /// The returned count is min(maxCount, remaining entities).
        /// </summary>
        public static ReadOnlySpan<uint> GetEntityIdSlice(ulong queryHandle,
            int archetypeIndex, int entityOffset, int maxCount)
        {
            EnsureLoaded();
            if (s_cachedQueryGetEntityIdSlice == null)
                throw new MissingMethodException("ECS ABI CachedQueryGetEntityIdSlice not available (v1.6 required)");

            int rc = s_cachedQueryGetEntityIdSlice(queryHandle,
                (uint)archetypeIndex, (uint)entityOffset, (uint)maxCount,
                out uint* entityIds, out uint count);
            if (rc != 0)
                throw new InvalidOperationException($"CachedQueryGetEntityIdSlice failed rc={rc}");

            if (entityIds == null || count == 0)
                return ReadOnlySpan<uint>.Empty;

            return new ReadOnlySpan<uint>(entityIds, (int)count);
        }
    }
}
