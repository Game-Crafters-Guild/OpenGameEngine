using System;
using NUnit.Framework;
using GameEngine.Interop;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// ABI handshake tests verifying version negotiation between managed and native sides.
    /// </summary>
    public class AbiHandshakeTests
    {
        /// <summary>
        /// Loads the native interface table using the expected ABI and validates the struct size.
        /// </summary>
        [Test]
        public void NativeAbi_Matching_Succeeds()
        {
            using var binding = EngineNativeBinding.LoadFrom(null);
            Assert.That(binding.Handle, Is.Not.EqualTo(0));
            Assert.That(binding.Iface.sizeBytes, Is.EqualTo((uint)System.Runtime.InteropServices.Marshal.SizeOf<GE_Interface_v1>()));
        }

        /// <summary>
        /// Requests an incompatible major ABI version and expects an exception from native negotiation.
        /// </summary>
        [Test]
        public void NativeAbi_MajorMismatch_Throws()
        {
            Assert.Throws<InvalidOperationException>(() => EngineNativeBinding.LoadFrom(null, abiOverride: (EngineNativeBinding.kAbiMajor + 1u) << 16));
        }
    }
}

