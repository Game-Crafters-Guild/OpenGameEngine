using System.Runtime.CompilerServices;
using GameEngine.ECS;
using GameEngine.ECS.Internal;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Buffers structural ECS operations for deferred execution.
    /// Pass as a parameter to IEntitySystem.Execute.
    /// Zero managed heap allocations — all data passed via stack pinning.
    /// </summary>
    public unsafe struct EntityCommands
    {
        internal ulong WorldHandle;

        /// <summary>Creates a new empty entity (no components). Returns a valid entity ID immediately.</summary>
        public uint CreateEntity()
        {
            return ChunkQueryNative.CreateEntityRaw(WorldHandle);
        }

        /// <summary>Queues entity destruction. Executes after all systems complete.</summary>
        public void DestroyEntity(uint entityId)
        {
            ChunkQueryNative.DeferCommand(WorldHandle, 1, entityId, 0, null, 0);
        }

        /// <summary>Queues adding a component. Executes after all systems complete.</summary>
        public void AddComponent<T>(uint entityId, in T component) where T : unmanaged, IComponent
        {
            fixed (T* ptr = &component)
            {
                ChunkQueryNative.DeferCommand(WorldHandle, 2, entityId,
                    ComponentType<T>.CachedId, ptr, (uint)Unsafe.SizeOf<T>());
            }
        }

        /// <summary>Queues removing a component. Executes after all systems complete.</summary>
        public void RemoveComponent<T>(uint entityId) where T : unmanaged, IComponent
        {
            ChunkQueryNative.DeferCommand(WorldHandle, 3, entityId,
                ComponentType<T>.CachedId, null, 0);
        }

        /// <summary>
        /// Queues switching the entity on or off. Executes after all systems complete. A disabled
        /// entity — and every entity under it in the hierarchy — is skipped by every system; an
        /// entity switched back on is visited again at once, unless an entity above it in the
        /// hierarchy is still off.
        /// </summary>
        public void SetEnabled(uint entityId, bool enabled)
        {
            ChunkQueryNative.SetEntityEnabled(WorldHandle, entityId, enabled);
        }

        /// <summary>
        /// Queues switching component T on or off on the entity without removing its data.
        /// Executes after all systems complete. Systems skip a disabled T as if it were absent.
        /// Throws <see cref="InvalidOperationException"/> for a component with no on/off state
        /// (a built-in such as Transform); switch the entity off instead.
        /// </summary>
        public void SetEnabled<T>(uint entityId, bool enabled) where T : unmanaged, IComponent
        {
            ulong disabledTypeId = ComponentType<T>.DisabledTypeId;
            if (disabledTypeId == 0)
                throw new InvalidOperationException(
                    $"Component '{typeof(T).Name}' is not toggleable: it has no on/off state and is always on. " +
                    "Switch the entity off with SetEnabled(entityId, false) instead.");
            QueueTag(entityId, disabledTypeId, !enabled);
        }

        private void QueueTag(uint entityId, ulong tagTypeId, bool present)
        {
            if (present)
            {
                byte tag = 0;
                ChunkQueryNative.DeferCommand(WorldHandle, 2, entityId, tagTypeId, &tag, 1);
            }
            else
            {
                ChunkQueryNative.DeferCommand(WorldHandle, 3, entityId, tagTypeId, null, 0);
            }
        }

        /// <summary>Queues setting/overwriting a component value. Executes after all systems complete.</summary>
        public void SetComponent<T>(uint entityId, in T component) where T : unmanaged, IComponent
        {
            fixed (T* ptr = &component)
            {
                ChunkQueryNative.DeferCommand(WorldHandle, 4, entityId,
                    ComponentType<T>.CachedId, ptr, (uint)Unsafe.SizeOf<T>());
            }
        }
    }
}
