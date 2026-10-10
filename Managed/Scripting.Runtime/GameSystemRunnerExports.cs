using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting.Runtime
{
    /// <summary>
    /// Native-callable exports for standalone builds.
    ///
    /// Under CoreCLR: ManagedSystemBridge resolves these via
    /// CoreCLRHost::GetManagedFunction() (by .NET type+method name).
    /// The EntryPoint strings are ignored.
    ///
    /// Under NativeAOT: These appear in the DLL export table by EntryPoint name.
    /// The Player calls LoadLibrary + GetProcAddress("ge_scripts_*").
    ///
    /// In editor builds, PlayModeDriver calls GameSystemRunner directly instead.
    /// </summary>
    public static class GameSystemRunnerExports
    {
        [UnmanagedCallersOnly(EntryPoint = "ge_scripts_initialize", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int NativeInitialize(ulong worldHandle)
        {
            try
            {
                GameSystemRunner.Initialize(worldHandle);
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[GameSystemRunner] NativeInitialize failed: {ex}");
                return -1;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "ge_scripts_tick", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int NativeTick(float deltaTime)
        {
            try
            {
                GameSystemRunner.Tick(deltaTime);
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[GameSystemRunner] NativeTick failed: {ex}");
                return -1;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "ge_scripts_shutdown", CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int NativeShutdown()
        {
            try
            {
                GameSystemRunner.Shutdown();
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[GameSystemRunner] NativeShutdown failed: {ex}");
                return -1;
            }
        }
    }
}
