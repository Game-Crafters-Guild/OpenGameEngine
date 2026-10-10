using System;
using System.Runtime.InteropServices;
using System.Text;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Managed façade over the native DebugMetrics service. Lets gameplay/editor
    /// scripts publish custom counters to the engine's Monitors panel.
    /// Names are slash-grouped, e.g. "Game/EnemiesAlive" appears under a "Game" group.
    /// </summary>
    public static class DebugMetrics
    {
        private const string kLib = "GameEngine.Native";

        public enum MonitorType : int
        {
            Quantity = 0,
            Memory = 1,
            TimeMs = 2,
            Percent = 3,
        }

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_DebugMetrics_RegisterMonitor(
            IntPtr nameUtf8, uint nameLen, int type, IntPtr unitUtf8, uint unitLen);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_DebugMetrics_PushSample(
            IntPtr nameUtf8, uint nameLen, float value);

        public static void RegisterMonitor(string name, MonitorType type, string? unit = null)
        {
            if (string.IsNullOrEmpty(name))
                return;
            byte[] nameBytes = Encoding.UTF8.GetBytes(name);
            byte[]? unitBytes = string.IsNullOrEmpty(unit) ? null : Encoding.UTF8.GetBytes(unit!);
            unsafe
            {
                fixed (byte* nameP = nameBytes)
                fixed (byte* unitP = unitBytes)
                {
                    GE_DebugMetrics_RegisterMonitor(
                        (IntPtr)nameP, (uint)nameBytes.Length,
                        (int)type,
                        (IntPtr)unitP, (uint)(unitBytes?.Length ?? 0));
                }
            }
        }

        public static void PushSample(string name, float value)
        {
            if (string.IsNullOrEmpty(name))
                return;
            byte[] nameBytes = Encoding.UTF8.GetBytes(name);
            unsafe
            {
                fixed (byte* nameP = nameBytes)
                {
                    GE_DebugMetrics_PushSample((IntPtr)nameP, (uint)nameBytes.Length, value);
                }
            }
        }
    }
}
