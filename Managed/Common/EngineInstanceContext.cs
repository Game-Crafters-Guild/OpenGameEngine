using System;

namespace GameEngine.Interop
{
    /// <summary>
    /// Managed-side context for one engine instance: the native engine library it talks to and
    /// the binding to that library, loaded lazily on first use.
    /// </summary>
    public sealed class EngineInstanceContext : IDisposable
    {
        /// <summary>Absolute path to this instance's native engine library.</summary>
        public string NativePath { get; }
        /// <summary>Optional ABI override for this context.</summary>
        private readonly uint? m_abiOverride;
        /// <summary>Native binding loaded from NativePath (lazy).</summary>
        private EngineNativeBinding? m_binding;
        /// <summary>Access the native binding (loads on first use).</summary>
        public EngineNativeBinding Binding => m_binding ??= EngineNativeBinding.LoadFrom(NativePath, abiOverride: m_abiOverride);

        /// <summary>
        /// Creates a new engine instance context; the native binding is loaded lazily on first access.
        /// </summary>
        public EngineInstanceContext(string nativePath, uint? abiOverride = null)
        {
            NativePath = nativePath;
            m_abiOverride = abiOverride;
        }

        /// <summary>Disposes the underlying native binding if created.</summary>
        public void Dispose()
        {
            try { m_binding?.Dispose(); } catch { }
            m_binding = null;
        }
    }
}
