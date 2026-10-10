using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Runtime.CompilerServices;

namespace GameEngine.ECS
{
    /// <summary>
    /// Built-in primitive types supported by the ECS ABI create API.
    /// </summary>
    public enum PrimitiveType : uint
    {
        Cube = 0,
        Sphere = 1,
        Capsule = 2,
        Plane = 3
    }

    /// <summary>
    /// Entry point for ECS access from scripts/tools.
    /// </summary>
    public static class Ecs
    {
        /// <summary>
        /// Gets the primary (default) world.
        /// </summary>
        public static WorldHandle PrimaryWorld
        {
            get
            {
                ulong handle = Native.GetPrimaryWorld();
                return new WorldHandle(handle);
            }
        }

        /// <summary>
        /// Registers a runtime-defined fixed-size blob component type.
        /// Returns the assigned 64-bit ComponentTypeId on success.
        /// When the source generator has cataloged a field schema for the name (see
        /// RegisterComponentSchema), the registration carries the field table across
        /// the ABI so the component is scene-serializable, inspectable, and migratable;
        /// otherwise it registers name+size only, exactly as before.
        /// </summary>
        public static ulong RegisterBlobComponent(string name, uint sizeBytes)
        {
            if (ComponentSchemaCatalog.TryGet(name, out var schema) && schema.SizeBytes == sizeBytes)
            {
                int src = Native.TryRegisterBlobComponentWithSchema(name, sizeBytes, schema.Fields, out var schemaTypeId);
                if (src == 0 && schemaTypeId != 0)
                    return schemaTypeId;
                // Fall through: an older native without the v2.1 entry, or a schema the
                // native side rejected — the component still works name+size only.
            }

            int rc = Native.TryRegisterBlobComponent(name, sizeBytes, out var typeId);
            if (rc != 0 || typeId == 0)
                throw new InvalidOperationException("Failed to register blob component");
            return typeId;
        }

        /// <summary>
        /// Catalogs a generator-emitted component field schema and eagerly registers the
        /// component (with its field table) in the native engine. Called from each script
        /// assembly's [ModuleInitializer] registration hub, so C# components are known to
        /// the scene loader and the inspector at assembly load — before any system runs.
        /// Never throws: in a host without the native engine the schema stays cataloged
        /// and the next RegisterBlobComponent call carries it across. Returns the type id,
        /// or 0 when the native engine is unavailable.
        /// </summary>
        public static ulong RegisterComponentSchema(string name, uint sizeBytes, ComponentFieldDesc[] fields)
        {
            if (string.IsNullOrEmpty(name) || sizeBytes == 0 || fields == null)
                return 0;

            ComponentSchemaCatalog.Put(name, sizeBytes, fields);

            try
            {
                int rc = Native.TryRegisterBlobComponentWithSchema(name, sizeBytes, fields, out var typeId);
                if (rc == 0 && typeId != 0)
                    return typeId;
                if (rc != 0)
                    Console.Error.WriteLine(
                        $"[Ecs] component schema registration for '{name}' failed rc={rc}; " +
                        "the component will register name+size only");
            }
            catch
            {
                // Native engine not loadable in this host (pure managed test run) —
                // the catalog entry keeps the schema for a later lazy registration.
            }
            return 0;
        }

        /// <summary>
        /// Looks up a component type ID by its native name. Returns the type ID if found.
        /// Used by source-generated code for [BuiltInComponent] types that are already
        /// registered in the C++ ComponentRegistry.
        /// </summary>
        public static ulong GetComponentTypeIdByName(string nativeName)
        {
            int rc = Native.TryGetComponentTypeIdByName(nativeName, out var typeId);
            if (rc != 0 || typeId == 0)
                throw new InvalidOperationException(
                    $"Built-in component '{nativeName}' not registered in native engine. " +
                    "Ensure the engine has initialized the component before C# systems run.");
            return typeId;
        }
    }

    /// <summary>
    /// Strongly-typed access to a runtime-defined blob component type.
    /// This is the recommended way to author custom ECS components in C# (V1).
    /// All members are static: the type id is resolved once per T. (The former
    /// instance form was a footgun — <c>new ComponentType&lt;T&gt;()</c> bound to
    /// the implicit default struct constructor and produced type id 0.)
    /// </summary>
    public static class ComponentType<T> where T : unmanaged
    {
        // Static cache: lazily registers on first access. RegisterBlobComponent is idempotent.
        private static ulong s_cachedId;
        private static ulong s_disabledTypeId;
        private static bool s_disabledTypeIdResolved;

        /// <summary>
        /// Returns the ComponentTypeId for T, lazily registering on first access.
        /// For types with [BuiltInComponent], resolves via the native name instead of
        /// registering a new blob component. Thread-safe because both paths are idempotent.
        /// </summary>
        public static ulong CachedId
        {
            get
            {
                ulong id = s_cachedId;
                if (id != 0) return id;
                id = ResolveTypeId();
                s_cachedId = id;
                return id;
            }
        }

        /// <summary>
        /// The type id of the tag that switches T off on an entity: adding it disables T,
        /// removing it enables T again. Systems skip a disabled T as if it were absent.
        /// Zero for a component with no on/off state (a built-in such as Transform), which
        /// is always on. Use <c>EntityCommands.SetEnabled&lt;T&gt;</c> and
        /// <c>WorldHandle.IsEnabled&lt;T&gt;</c> rather than this id directly.
        /// </summary>
        public static ulong DisabledTypeId
        {
            get
            {
                if (Volatile.Read(ref s_disabledTypeIdResolved))
                    return s_disabledTypeId;
                int rc = Native.TryGetComponentDisabledTypeId(CachedId, out ulong id);
                if (rc != 0)
                    throw new InvalidOperationException(
                        $"Could not resolve the disabled tag of component '{RegistrationName()}' (rc={rc}).");
                s_disabledTypeId = id;
                Volatile.Write(ref s_disabledTypeIdResolved, true);
                return id;
            }
        }

        private static ulong ResolveTypeId()
        {
            // Check if T has [BuiltInComponent("nativeName")] — resolve via native name.
            // Uses reflection by attribute name to avoid a circular reference to Scripting.ABI.
            foreach (var attr in typeof(T).GetCustomAttributes(false))
            {
                var attrType = attr.GetType();
                if (attrType.FullName == "GameEngine.Scripting.BuiltInComponentAttribute")
                {
                    var prop = attrType.GetProperty("NativeName");
                    if (prop?.GetValue(attr) is string nativeName)
                        return Ecs.GetComponentTypeIdByName(nativeName);
                }
            }

            return Ecs.RegisterBlobComponent(RegistrationName(), (uint)Unsafe.SizeOf<T>());
        }

        /// <summary>
        /// The name a C# component registers under: the CLR namespace-qualified name
        /// with nested-type '+' normalized to '.' so it matches the source generator's
        /// ToDisplayString form. Namespaces disambiguate same-simple-name components
        /// across assemblies, exactly like native's normalized-qualified-name hashing.
        /// </summary>
        internal static string RegistrationName()
        {
            string name = typeof(T).FullName ?? typeof(T).Name;
            return name.Replace('+', '.');
        }

        /// <summary>Sets component value on the entity (adds the component if missing).</summary>
        public static void Set(in WorldHandle world, uint entityId, in T value)
        {
            Span<byte> tmp = stackalloc byte[Unsafe.SizeOf<T>()];
            MemoryMarshal.Write(tmp, in value);
            world.SetComponentBytes(entityId, CachedId, tmp);
        }

        /// <summary>Reads component value from the entity.</summary>
        public static T Get(in WorldHandle world, uint entityId)
        {
            int size = Unsafe.SizeOf<T>();
            Span<byte> tmp = stackalloc byte[size];
            if (!world.TryGetComponentBytesInto(entityId, CachedId, tmp, out uint written) || written != (uint)size)
                throw new InvalidOperationException("Failed to read component bytes (size mismatch or missing)");
            return MemoryMarshal.Read<T>(tmp);
        }
    }

    /// <summary>
    /// ECS world handle façade. Provides basic queries and creation helpers.
    /// </summary>
    public readonly struct WorldHandle
    {
        private readonly ulong m_handle;

        internal WorldHandle(ulong handle) { m_handle = handle; }

        /// <summary>Opaque native world handle.</summary>
        public ulong Handle => m_handle;

        /// <summary>Gets the number of entities in this world.</summary>
        public int EntityCount
        {
            get
            {
                if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
                if (Native.TryGetEntityCount(m_handle, out int count) != 0)
                    throw new InvalidOperationException("Failed to query entity count");
                return count;
            }
        }

        /// <summary>Try to get the number of entities in this world. Returns false on failure.</summary>
        public bool TryGetEntityCount(out int count)
        {
            count = 0;
            if (m_handle == 0) return false;
            return Native.TryGetEntityCount(m_handle, out count) == 0;
        }

        /// <summary>
        /// Creates an empty entity (Transform + Name), optionally parented.
        /// Returns the packed entity id (index+version).
        /// </summary>
        public uint CreateEmptyEntity(string name, uint parentEntityId = 0)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            if (Native.TryCreateEmptyEntity(m_handle, name ?? string.Empty, parentEntityId, out uint entity) != 0)
                throw new InvalidOperationException("Failed to create entity");
            return entity;
        }

        /// <summary>
        /// Creates a primitive entity (Transform + Name + MeshRenderer), optionally parented.
        /// Returns the packed entity id (index+version).
        /// </summary>
        public uint CreatePrimitive(PrimitiveType primitive, string name, uint parentEntityId = 0)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            if (Native.TryCreatePrimitive(m_handle, primitive, name ?? string.Empty, parentEntityId, out uint entity) != 0)
                throw new InvalidOperationException("Failed to create primitive");
            return entity;
        }

        /// <summary>Returns true if the entity handle is currently valid/alive in this world.</summary>
        public bool IsEntityValid(uint entityId)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            if (Native.TryIsEntityValid(m_handle, entityId, out bool valid) != 0)
                return false;
            return valid;
        }

        /// <summary>
        /// Destroys an entity. Deferred, mirroring native <c>World::DestroyEntity</c>:
        /// the destroy is queued on the world's command buffer and applied at the next
        /// flush (end of the system tick), so <see cref="IsEntityValid"/> keeps
        /// returning true until then.
        /// </summary>
        /// <remarks>
        /// Inside a system, prefer <c>EntityCommands.DestroyEntity</c> — it queues into
        /// the same deferred stream that is flushed after all systems complete. Native
        /// falls back to immediate destruction only when the command buffer is full and
        /// structural-change deferral is not active.
        /// </remarks>
        public void DestroyEntity(uint entityId)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            if (Native.TryDestroyEntity(m_handle, entityId) != 0)
                throw new InvalidOperationException("Failed to destroy entity");
        }

        /// <summary>Sets the Name component (creates it if missing).</summary>
        public void SetName(uint entityId, string name)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            if (Native.TrySetName(m_handle, entityId, name ?? string.Empty) != 0)
                throw new InvalidOperationException("Failed to set Name");
        }

        /// <summary>Gets the Name component. Throws if missing.</summary>
        public string GetName(uint entityId)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            int rc = Native.TryGetName(m_handle, entityId, out string name);
            if (rc != 0)
                throw new InvalidOperationException("Failed to get Name");
            return name;
        }

        /// <summary>Sets the local Transform matrix (column-major, 16 floats). Creates Transform if missing.</summary>
        public void SetTransformMatrix(uint entityId, ReadOnlySpan<float> matrix16)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            if (matrix16.Length < 16) throw new ArgumentException("matrix16 must contain at least 16 floats", nameof(matrix16));
            int rc = Native.TrySetTransform(m_handle, entityId, matrix16);
            if (rc != 0)
                throw new InvalidOperationException("Failed to set Transform");
        }

        /// <summary>Gets the local Transform matrix (column-major, 16 floats). Throws if missing.</summary>
        public float[] GetTransformMatrix(uint entityId)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            int rc = Native.TryGetTransform(m_handle, entityId, out float[] m);
            if (rc != 0)
                throw new InvalidOperationException("Failed to get Transform");
            return m;
        }

        /// <summary>Sets an arbitrary component (identified by ComponentTypeId) from raw bytes.</summary>
        public void SetComponentBytes(uint entityId, ulong componentTypeId, ReadOnlySpan<byte> bytes)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            int rc = Native.TrySetComponentBytes(m_handle, entityId, componentTypeId, bytes);
            if (rc != 0)
                throw new InvalidOperationException("Failed to set component bytes");
        }

        /// <summary>Gets an arbitrary component (identified by ComponentTypeId) as raw bytes.</summary>
        public byte[] GetComponentBytes(uint entityId, ulong componentTypeId)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            int rc = Native.TryGetComponentBytes(m_handle, entityId, componentTypeId, out var bytes);
            if (rc != 0)
                throw new InvalidOperationException("Failed to get component bytes");
            return bytes;
        }

        /// <summary>
        /// Tries to read an arbitrary component (identified by ComponentTypeId) into a caller-provided buffer.
        /// This is allocation-free and intended for tight loops.
        /// </summary>
        public bool TryGetComponentBytesInto(uint entityId, ulong componentTypeId, Span<byte> destination, out uint writtenBytes)
        {
            writtenBytes = 0;
            if (m_handle == 0) return false;
            int rc = Native.TryGetComponentBytesInto(m_handle, entityId, componentTypeId, destination, out writtenBytes);
            return rc == 0;
        }

        /// <summary>Returns true if the entity has a component of the given type.</summary>
        public bool HasComponent(uint entityId, ulong componentTypeId)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            return Internal.ChunkQueryNative.HasComponent(m_handle, entityId, componentTypeId);
        }

        /// <summary>
        /// False when component T is switched off on the entity (<c>EntityCommands.SetEnabled&lt;T&gt;</c>).
        /// A component with no on/off state (a built-in such as Transform) is always on.
        /// The entity's own enabled state is separate.
        /// </summary>
        public bool IsEnabled<T>(uint entityId) where T : unmanaged
        {
            ulong disabledTypeId = ComponentType<T>.DisabledTypeId;
            return disabledTypeId == 0 || !HasComponent(entityId, disabledTypeId);
        }

        /// <summary>Removes an arbitrary component (identified by ComponentTypeId).</summary>
        public void RemoveComponent(uint entityId, ulong componentTypeId)
        {
            if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
            int rc = Native.TryRemoveComponent(m_handle, entityId, componentTypeId);
            if (rc != 0)
                throw new InvalidOperationException("Failed to remove component");
        }

    }

    internal static unsafe class Native
    {
        // Mirror of the native GE_ECS_FieldDesc (ECSABI.h): pointer, two uints, a uint,
        // two ushorts — 24 bytes on x64 with natural alignment on both sides.
        [StructLayout(LayoutKind.Sequential)]
        private struct GE_ECS_FieldDesc
        {
            public byte* nameUtf8;
            public uint nameLen;
            public uint offset;
            public uint size;
            public ushort fieldType;
            public ushort reserved;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_GetPrimaryWorld_Delegate(out ulong world);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_GetEntityCount_Delegate(ulong world, out int count);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_CreateEmptyEntity_Delegate(ulong world, byte* nameUtf8, uint nameLen, uint parentEntity, out uint entity);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_CreatePrimitive_Delegate(ulong world, PrimitiveType primitive, byte* nameUtf8, uint nameLen, uint parentEntity, out uint entity);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_IsEntityValid_Delegate(ulong world, uint entity, out int valid);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_DestroyEntity_Delegate(ulong world, uint entity);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_SetName_Delegate(ulong world, uint entity, byte* nameUtf8, uint nameLen);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_GetName_Delegate(ulong world, uint entity, byte* outNameUtf8, uint outCap, out uint outLen);

        [StructLayout(LayoutKind.Sequential)]
        private struct GE_ECS_Transform
        {
            public fixed float matrix[16];
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_GetTransform_Delegate(ulong world, uint entity, out GE_ECS_Transform transform);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_SetTransform_Delegate(ulong world, uint entity, in GE_ECS_Transform transform);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_RegisterBlobComponent_Delegate(byte* nameUtf8, uint nameLen, uint sizeBytes, out ulong typeId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_RegisterBlobComponentWithSchema_Delegate(
            byte* nameUtf8, uint nameLen, uint sizeBytes,
            GE_ECS_FieldDesc* fields, uint fieldCount, out ulong typeId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_GetComponentTypeIdByName_Delegate(byte* nameUtf8, uint nameLen, out ulong typeId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_GetComponentDisabledTypeId_Delegate(ulong componentTypeId, out ulong disabledTypeId);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_SetComponentBytes_Delegate(ulong world, uint entity, ulong componentTypeId, byte* data, uint dataLen);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_GetComponentBytes_Delegate(ulong world, uint entity, ulong componentTypeId, byte* outData, uint outCap, out uint outLen);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_ECSABI_RemoveComponent_Delegate(ulong world, uint entity, ulong componentTypeId);

        private static readonly object s_lock = new object();
        private static bool s_loaded;

        private static GE_ECSABI_GetPrimaryWorld_Delegate? s_getPrimaryWorld;
        private static GE_ECSABI_GetEntityCount_Delegate? s_getEntityCount;
        private static GE_ECSABI_CreateEmptyEntity_Delegate? s_createEmptyEntity;
        private static GE_ECSABI_CreatePrimitive_Delegate? s_createPrimitive;

        private static GE_ECSABI_IsEntityValid_Delegate? s_isEntityValid;
        private static GE_ECSABI_DestroyEntity_Delegate? s_destroyEntity;
        private static GE_ECSABI_SetName_Delegate? s_setName;
        private static GE_ECSABI_GetName_Delegate? s_getName;
        private static GE_ECSABI_GetTransform_Delegate? s_getTransform;
        private static GE_ECSABI_SetTransform_Delegate? s_setTransform;

        private static GE_ECSABI_RegisterBlobComponent_Delegate? s_registerBlob;
        private static GE_ECSABI_RegisterBlobComponentWithSchema_Delegate? s_registerBlobWithSchema;
        private static GE_ECSABI_GetComponentTypeIdByName_Delegate? s_getTypeIdByName;
        private static GE_ECSABI_GetComponentDisabledTypeId_Delegate? s_getDisabledTypeId;
        private static GE_ECSABI_SetComponentBytes_Delegate? s_setComponentBytes;
        private static GE_ECSABI_GetComponentBytes_Delegate? s_getComponentBytes;
        private static GE_ECSABI_RemoveComponent_Delegate? s_removeComponent;

        private static void EnsureLoaded()
        {
            if (s_loaded)
                return;

            lock (s_lock)
            {
                if (s_loaded)
                    return;

                var iface = Internal.EcsInterfaceTable.Acquire();

                s_getPrimaryWorld = iface.GetPrimaryWorld != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_GetPrimaryWorld_Delegate>(iface.GetPrimaryWorld) : null;
                s_getEntityCount = iface.GetEntityCount != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_GetEntityCount_Delegate>(iface.GetEntityCount) : null;
                s_createEmptyEntity = iface.CreateEmptyEntity != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_CreateEmptyEntity_Delegate>(iface.CreateEmptyEntity) : null;
                s_createPrimitive = iface.CreatePrimitive != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_CreatePrimitive_Delegate>(iface.CreatePrimitive) : null;

                s_isEntityValid = iface.IsEntityValid != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_IsEntityValid_Delegate>(iface.IsEntityValid) : null;
                s_destroyEntity = iface.DestroyEntity != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_DestroyEntity_Delegate>(iface.DestroyEntity) : null;
                s_setName = iface.SetName != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_SetName_Delegate>(iface.SetName) : null;
                s_getName = iface.GetName != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_GetName_Delegate>(iface.GetName) : null;
                s_getTransform = iface.GetTransform != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_GetTransform_Delegate>(iface.GetTransform) : null;
                s_setTransform = iface.SetTransform != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_SetTransform_Delegate>(iface.SetTransform) : null;

                s_registerBlob = iface.RegisterBlobComponent != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_RegisterBlobComponent_Delegate>(iface.RegisterBlobComponent) : null;
                s_registerBlobWithSchema = iface.RegisterBlobComponentWithSchema != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_RegisterBlobComponentWithSchema_Delegate>(iface.RegisterBlobComponentWithSchema) : null;
                s_getTypeIdByName = iface.GetComponentTypeIdByName != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_GetComponentTypeIdByName_Delegate>(iface.GetComponentTypeIdByName) : null;
                s_getDisabledTypeId = iface.GetComponentDisabledTypeId != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_GetComponentDisabledTypeId_Delegate>(iface.GetComponentDisabledTypeId) : null;
                s_setComponentBytes = iface.SetComponentBytes != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_SetComponentBytes_Delegate>(iface.SetComponentBytes) : null;
                s_getComponentBytes = iface.GetComponentBytes != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_GetComponentBytes_Delegate>(iface.GetComponentBytes) : null;
                s_removeComponent = iface.RemoveComponent != 0 ? Marshal.GetDelegateForFunctionPointer<GE_ECSABI_RemoveComponent_Delegate>(iface.RemoveComponent) : null;

                s_loaded = true;
            }
        }

        public static ulong GetPrimaryWorld()
        {
            EnsureLoaded();
            if (s_getPrimaryWorld == null)
                throw new MissingMethodException("ECS ABI GetPrimaryWorld not available");
            int rc = s_getPrimaryWorld(out ulong world);
            if (rc != 0 || world == 0)
                throw new InvalidOperationException("Primary world not available");
            return world;
        }

        public static int TryGetEntityCount(ulong world, out int count)
        {
            EnsureLoaded();
            count = 0;
            if (s_getEntityCount == null)
                return -1;
            return s_getEntityCount(world, out count);
        }

        public static int TryCreateEmptyEntity(ulong world, string name, uint parentEntity, out uint entity)
        {
            EnsureLoaded();
            entity = 0;
            if (s_createEmptyEntity == null)
                return -1;

            byte[] bytes = string.IsNullOrEmpty(name) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(name);
            if (bytes.Length == 0)
            {
                return s_createEmptyEntity(world, null, 0, parentEntity, out entity);
            }
            fixed (byte* p = bytes)
            {
                return s_createEmptyEntity(world, p, (uint)bytes.Length, parentEntity, out entity);
            }
        }

        public static int TryCreatePrimitive(ulong world, PrimitiveType primitive, string name, uint parentEntity, out uint entity)
        {
            EnsureLoaded();
            entity = 0;
            if (s_createPrimitive == null)
                return -1;

            byte[] bytes = string.IsNullOrEmpty(name) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(name);
            if (bytes.Length == 0)
            {
                return s_createPrimitive(world, primitive, null, 0, parentEntity, out entity);
            }
            fixed (byte* p = bytes)
            {
                return s_createPrimitive(world, primitive, p, (uint)bytes.Length, parentEntity, out entity);
            }
        }

        public static int TryIsEntityValid(ulong world, uint entity, out bool valid)
        {
            EnsureLoaded();
            valid = false;
            if (s_isEntityValid == null)
                return -1;
            int rc = s_isEntityValid(world, entity, out int v);
            valid = (v != 0);
            return rc;
        }

        public static int TryDestroyEntity(ulong world, uint entity)
        {
            EnsureLoaded();
            if (s_destroyEntity == null)
                return -1;
            return s_destroyEntity(world, entity);
        }

        public static int TrySetName(ulong world, uint entity, string name)
        {
            EnsureLoaded();
            if (s_setName == null)
                return -1;
            byte[] bytes = string.IsNullOrEmpty(name) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(name);
            if (bytes.Length == 0)
            {
                return s_setName(world, entity, null, 0);
            }
            fixed (byte* p = bytes)
            {
                return s_setName(world, entity, p, (uint)bytes.Length);
            }
        }

        public static int TryGetName(ulong world, uint entity, out string name)
        {
            EnsureLoaded();
            name = string.Empty;
            if (s_getName == null)
                return -1;

            // First query required length.
            int rcLen = s_getName(world, entity, null, 0, out uint len);
            if (rcLen != 0)
                return rcLen;
            if (len == 0)
            {
                name = string.Empty;
                return 0;
            }

            // Allocate len+1 so native can null-terminate.
            byte[] buf = new byte[len + 1];
            fixed (byte* p = buf)
            {
                int rc = s_getName(world, entity, p, (uint)buf.Length, out uint writtenLen);
                if (rc != 0)
                    return rc;
                uint actual = Math.Min(writtenLen, len);
                name = Encoding.UTF8.GetString(buf, 0, (int)actual);
                return 0;
            }
        }

        public static int TryGetTransform(ulong world, uint entity, out float[] matrix16)
        {
            EnsureLoaded();
            matrix16 = new float[16];
            if (s_getTransform == null)
                return -1;
            int rc = s_getTransform(world, entity, out GE_ECS_Transform t);
            if (rc != 0)
                return rc;
            for (int i = 0; i < 16; i++)
            {
                matrix16[i] = t.matrix[i];
            }
            return 0;
        }

        public static int TrySetTransform(ulong world, uint entity, ReadOnlySpan<float> matrix16)
        {
            EnsureLoaded();
            if (s_setTransform == null)
                return -1;
            if (matrix16.Length < 16)
                return -2;

            GE_ECS_Transform t = default;
            for (int i = 0; i < 16; i++)
                t.matrix[i] = matrix16[i];
            return s_setTransform(world, entity, in t);
        }

        public static int TryRegisterBlobComponent(string name, uint sizeBytes, out ulong typeId)
        {
            EnsureLoaded();
            typeId = 0;
            if (s_registerBlob == null)
                return -1;
            byte[] bytes = string.IsNullOrEmpty(name) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(name);
            if (bytes.Length == 0)
                return -2;
            fixed (byte* p = bytes)
            {
                return s_registerBlob(p, (uint)bytes.Length, sizeBytes, out typeId);
            }
        }

        public static int TryRegisterBlobComponentWithSchema(string name, uint sizeBytes,
            ComponentFieldDesc[] fields, out ulong typeId)
        {
            EnsureLoaded();
            typeId = 0;
            if (s_registerBlobWithSchema == null)
                return -1;
            if (fields == null)
                return -2;
            byte[] nameBytes = string.IsNullOrEmpty(name) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(name);
            if (nameBytes.Length == 0)
                return -2;

            // All field names go into one UTF-8 pool so a single fixed statement pins them;
            // the native side copies everything before returning.
            int poolLen = 0;
            var nameOffsets = new int[fields.Length];
            var nameLengths = new int[fields.Length];
            for (int i = 0; i < fields.Length; i++)
            {
                var fieldName = fields[i].Name ?? string.Empty;
                nameOffsets[i] = poolLen;
                nameLengths[i] = Encoding.UTF8.GetByteCount(fieldName);
                poolLen += nameLengths[i];
            }
            var pool = new byte[Math.Max(1, poolLen)];
            for (int i = 0; i < fields.Length; i++)
            {
                var fieldName = fields[i].Name ?? string.Empty;
                Encoding.UTF8.GetBytes(fieldName, 0, fieldName.Length, pool, nameOffsets[i]);
            }

            var descs = new GE_ECS_FieldDesc[Math.Max(1, fields.Length)];
            fixed (byte* pName = nameBytes)
            fixed (byte* pPool = pool)
            fixed (GE_ECS_FieldDesc* pDescs = descs)
            {
                for (int i = 0; i < fields.Length; i++)
                {
                    pDescs[i].nameUtf8 = pPool + nameOffsets[i];
                    pDescs[i].nameLen = (uint)nameLengths[i];
                    pDescs[i].offset = fields[i].Offset;
                    pDescs[i].size = fields[i].Size;
                    pDescs[i].fieldType = (ushort)fields[i].Type;
                    pDescs[i].reserved = 0;
                }
                return s_registerBlobWithSchema(pName, (uint)nameBytes.Length, sizeBytes,
                    fields.Length == 0 ? null : pDescs, (uint)fields.Length, out typeId);
            }
        }

        public static int TryGetComponentTypeIdByName(string name, out ulong typeId)
        {
            EnsureLoaded();
            typeId = 0;
            if (s_getTypeIdByName == null)
                return -1;
            byte[] bytes = string.IsNullOrEmpty(name) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(name);
            if (bytes.Length == 0)
                return -2;
            fixed (byte* p = bytes)
            {
                return s_getTypeIdByName(p, (uint)bytes.Length, out typeId);
            }
        }

        public static int TryGetComponentDisabledTypeId(ulong componentTypeId, out ulong disabledTypeId)
        {
            EnsureLoaded();
            disabledTypeId = 0;
            if (s_getDisabledTypeId == null)
                return -1;
            return s_getDisabledTypeId(componentTypeId, out disabledTypeId);
        }

        public static int TrySetComponentBytes(ulong world, uint entity, ulong componentTypeId, ReadOnlySpan<byte> data)
        {
            EnsureLoaded();
            if (s_setComponentBytes == null)
                return -1;
            if (data.Length == 0)
                return s_setComponentBytes(world, entity, componentTypeId, null, 0);
            fixed (byte* p = data)
            {
                return s_setComponentBytes(world, entity, componentTypeId, p, (uint)data.Length);
            }
        }

        public static int TryGetComponentBytes(ulong world, uint entity, ulong componentTypeId, out byte[] bytes)
        {
            EnsureLoaded();
            bytes = Array.Empty<byte>();
            if (s_getComponentBytes == null)
                return -1;

            int rcLen = s_getComponentBytes(world, entity, componentTypeId, null, 0, out uint len);
            if (rcLen != 0)
                return rcLen;
            if (len == 0)
            {
                bytes = Array.Empty<byte>();
                return 0;
            }

            bytes = new byte[len];
            fixed (byte* p = bytes)
            {
                int rc = s_getComponentBytes(world, entity, componentTypeId, p, len, out _);
                return rc;
            }
        }

        public static int TryGetComponentBytesInto(ulong world, uint entity, ulong componentTypeId, Span<byte> destination, out uint writtenBytes)
        {
            EnsureLoaded();
            writtenBytes = 0;
            if (s_getComponentBytes == null)
                return -1;

            if (destination.Length == 0)
            {
                // Query required length only.
                return s_getComponentBytes(world, entity, componentTypeId, null, 0, out writtenBytes);
            }

            fixed (byte* p = destination)
            {
                return s_getComponentBytes(world, entity, componentTypeId, p, (uint)destination.Length, out writtenBytes);
            }
        }

        public static int TryRemoveComponent(ulong world, uint entity, ulong componentTypeId)
        {
            EnsureLoaded();
            if (s_removeComponent == null)
                return -1;
            return s_removeComponent(world, entity, componentTypeId);
        }
    }
}


