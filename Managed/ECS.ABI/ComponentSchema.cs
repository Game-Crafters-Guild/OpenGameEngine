using System;
using System.Collections.Generic;

namespace GameEngine.ECS
{
    /// <summary>
    /// Field type of a reflected component field. Mirrors the native ECS FieldTypeId
    /// taxonomy (ECS/Reflection.h) — a stable, append-only value contract shared by the
    /// scene serializer, the inspector, and hot-reload layout migration. Never renumber.
    /// </summary>
    public enum ComponentFieldType : ushort
    {
        Unknown = 0,
        Bool = 1,
        Int8 = 2,
        Int16 = 3,
        Int32 = 4,
        Int64 = 5,
        UInt8 = 6,
        UInt16 = 7,
        UInt32 = 8,
        UInt64 = 9,
        Float = 10,
        Double = 11,
        Vec2 = 12,
        Vec3 = 13,
        Vec4 = 14,
        Quat = 15,
        Mat4 = 16,
        Color = 17,
        AssetGuid = 18,
        EntityHandle = 19,
        String = 20,
        Bytes = 21,
    }

    /// <summary>
    /// One reflected field of a C# component, as emitted by the source generator.
    /// Offset/Size describe the field's byte span within the struct's MANAGED
    /// (LayoutKind.Sequential) layout — the exact bytes chunk spans and
    /// Set/GetComponentBytes touch. The generator computes them at build time and
    /// the native side validates every span against the component size.
    /// </summary>
    public readonly struct ComponentFieldDesc
    {
        public readonly string Name;
        public readonly uint Offset;
        public readonly uint Size;
        public readonly ComponentFieldType Type;

        public ComponentFieldDesc(string name, uint offset, uint size, ComponentFieldType type)
        {
            Name = name;
            Offset = offset;
            Size = size;
            Type = type;
        }
    }

    /// <summary>
    /// Name-keyed store of generator-emitted component schemas. Filled by each script
    /// assembly's [ModuleInitializer] registration hub; read by Ecs.RegisterBlobComponent
    /// so every blob registration path (generated systems, ComponentType&lt;T&gt;) carries
    /// its field table across the ABI. Keyed by the dotted registration name (never by
    /// System.Type) so entries hold no reference into a reloadable AssemblyLoadContext.
    /// </summary>
    internal static class ComponentSchemaCatalog
    {
        internal readonly struct Entry
        {
            public readonly uint SizeBytes;
            public readonly ComponentFieldDesc[] Fields;

            public Entry(uint sizeBytes, ComponentFieldDesc[] fields)
            {
                SizeBytes = sizeBytes;
                Fields = fields;
            }
        }

        private static readonly object s_lock = new object();
        private static readonly Dictionary<string, Entry> s_entries = new Dictionary<string, Entry>();

        /// <summary>Adds or replaces (hot-reload re-registers per assembly load) a schema.</summary>
        public static void Put(string name, uint sizeBytes, ComponentFieldDesc[] fields)
        {
            lock (s_lock)
            {
                s_entries[name] = new Entry(sizeBytes, fields);
            }
        }

        public static bool TryGet(string name, out Entry entry)
        {
            lock (s_lock)
            {
                return s_entries.TryGetValue(name, out entry);
            }
        }
    }
}
