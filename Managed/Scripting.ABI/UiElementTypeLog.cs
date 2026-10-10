using System;

namespace GameEngine.Scripting
{
    /// <summary>
    /// One diagnostic channel for the element-type machinery, so discovery and value binding can
    /// report without depending on the staging class that owns the tables.
    /// <para>
    /// Console rather than the engine logger on purpose: this runs on load and reload threads,
    /// including before the engine's managed logging is bound, and a diagnostic must never be the
    /// thing that throws.
    /// </para>
    /// </summary>
    internal static class UiElementTypeLog
    {
        internal static void Report(string message)
        {
            try { Console.WriteLine("[Ui.ElementTypes] " + message); }
            catch { }
        }
    }
}
