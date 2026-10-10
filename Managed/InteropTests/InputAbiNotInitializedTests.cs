using System.Runtime.InteropServices;
using NUnit.Framework;
using GameEngine.Interop;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// Validates the input ABI returns NotInitialized when no host Application is running.
    /// This keeps the ABI safe for headless test harnesses while Editor/game hosts wire a real InputSystem.
    /// </summary>
    public class InputAbiNotInitializedTests
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct GE_InputActionState
        {
            public byte pressed, justPressed, justReleased, _pad0;
            public float value;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_Input_GetActionState_Delegate(ulong actionId, out GE_InputActionState state);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_Input_RegisterAction_Delegate(ulong contextId, ulong actionId, int isAxis);

        /// <summary>
        /// Without an Application-owned InputSystem, we expect NotInitialized (-4) rather than a crash.
        /// </summary>
        [Test]
        public void InputAbi_WithoutApplication_ReturnsNotInitialized()
        {
            using var binding = EngineNativeBinding.LoadFrom(null);
            Assert.That(binding.Handle, Is.Not.EqualTo(0));

            if (!NativeLibrary.TryGetExport(binding.Handle, "GE_Input_GetActionState", out var pGetAction))
                Assert.Inconclusive("Loaded native binary does not export GE_Input_GetActionState (shim/older build).");
            if (!NativeLibrary.TryGetExport(binding.Handle, "GE_Input_RegisterAction", out var pRegister))
                Assert.Inconclusive("Loaded native binary does not export GE_Input_RegisterAction (shim/older build).");

            var getAction = Marshal.GetDelegateForFunctionPointer<GE_Input_GetActionState_Delegate>(pGetAction);
            var register = Marshal.GetDelegateForFunctionPointer<GE_Input_RegisterAction_Delegate>(pRegister);

            var rcState = getAction(123u, out _);
            Assert.That(rcState, Is.EqualTo(-4)); // GE_Result_NotInitialized

            var rcReg = register(1u, 2u, 0);
            Assert.That(rcReg, Is.EqualTo(-4)); // GE_Result_NotInitialized
        }
    }
}

