using System;
using GameEngine.Interop;

namespace GameEngine
{
    /// <summary>
    /// Entry point to managed engine APIs. Use Engine.Current to access context-scoped facades.
    /// </summary>
    public static class Engine
    {
        private static EngineInstanceContext? s_appLocalInstance;

        /// <summary>
        /// Gets the current EngineContext: the engine instance the native host registered, or,
        /// when no host registered one, the native library beside this application.
        /// </summary>
        public static EngineContext Current
        {
            get
            {
                var registered = GameEngine.CoreBridge.CoreBridge.GetEngineInstance();
                if (registered != null)
                {
                    return new EngineContext(registered);
                }
                return new EngineContext(System.Threading.LazyInitializer.EnsureInitialized(ref s_appLocalInstance, CreateAppLocalInstance));
            }
        }

        private static EngineInstanceContext CreateAppLocalInstance()
        {
            return new EngineInstanceContext(System.IO.Path.Combine(AppContext.BaseDirectory, GameEngine.Interop.NativeEngineLibrary.FileName));
        }
    }

    /// <summary>
    /// Provides access to engine subsystems scoped to a specific EngineInstanceContext.
    /// </summary>
    public readonly struct EngineContext
    {
        internal readonly EngineInstanceContext m_ctx;
        internal EngineContext(EngineInstanceContext ctx) { m_ctx = ctx; }
/// <summary>
/// Provides ECS-related access scoped to this engine context.
/// </summary>

        public WorldsProvider Worlds => new WorldsProvider(m_ctx);
    }

    /// <summary>
    /// Accessor for ECS worlds (cached between calls, invalidated on reloads).
    /// </summary>
    public readonly struct WorldsProvider
    {
        private readonly EngineInstanceContext m_ctx;
        private static readonly System.Collections.Concurrent.ConcurrentDictionary<EngineInstanceContext, ulong> s_worldCache = new();
/// <summary>
/// Gets the primary (default) world for this engine instance.
/// </summary>

        public WorldsProvider(EngineInstanceContext ctx) { m_ctx = ctx; }
        /// <summary>Gets the primary (default) world for this engine instance.</summary>
        public GameEngine.ECS.World Primary
        {
            get
            {
                if (!s_worldCache.TryGetValue(m_ctx, out var handle) || handle == 0)
                {
                    if (m_ctx.Binding.TryGetPrimaryWorld(out handle) != 0)
                        throw new InvalidOperationException("Primary world not available");
                    s_worldCache[m_ctx] = handle;
                }
                return new GameEngine.ECS.World(m_ctx, handle);
            }
        }
        internal static void Invalidate(GameEngine.Interop.EngineInstanceContext ctx) { s_worldCache.TryRemove(ctx, out _); }
    }
}

namespace GameEngine.ECS
{
    /// <summary>
    /// ECS world handle façade. Provides basic read-only queries.
    /// </summary>
    public readonly struct World
    {
        internal readonly EngineInstanceContext m_ctx;
        internal readonly ulong m_handle;
        internal World(EngineInstanceContext ctx, ulong handle) { m_ctx = ctx; m_handle = handle; }
/// <summary>
/// Gets the number of entities in this world.
/// </summary>

        public int EntityCount
        {
            get
            {
                if (m_handle == 0) throw new InvalidOperationException("World handle is invalid");
                if (m_ctx.Binding.TryGetEntityCount(m_handle, out int count) != 0)
                    throw new InvalidOperationException("Failed to query entity count");
                return count;
            }
        }

        /// <summary>Try to get the number of entities in this world. Returns false on failure.</summary>
        public bool TryGetEntityCount(out int count)
        {
            count = 0;
            if (m_handle == 0) return false;
            return m_ctx.Binding.TryGetEntityCount(m_handle, out count) == 0;
        }
    }
}

