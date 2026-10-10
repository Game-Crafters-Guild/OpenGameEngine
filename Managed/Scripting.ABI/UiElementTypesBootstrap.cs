using System.Runtime.CompilerServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Arms C#-defined UI element types as soon as this assembly is loaded.
    /// <para>
    /// The registration machinery has a bootstrap problem that no existing extension point
    /// solves: the orchestrator announces a completed reload, but its announcement carries no
    /// payload (<c>Action&lt;long, int, int&gt;</c>), and the accessor that would hand back the
    /// new assembly — <c>HotReloadManager.GetCurrentAssembly</c> — is compiled out of Release.
    /// Nothing, therefore, ever calls
    /// <see cref="UiElementTypes.StageTypesFrom"/> on its own.
    /// </para>
    /// <para>
    /// A module initializer closes it without the orchestrator growing an API. This assembly is
    /// pre-loaded into the default context and never unloaded, so this runs EXACTLY ONCE per
    /// process — it arms the subscription, and the subscription covers every later reload.
    /// </para>
    /// <para>
    /// <b>What makes it run is the orchestrator forcing it, not a user script referencing it.</b>
    /// A module initializer is guaranteed only at-or-before first ACCESS to a member of its
    /// module, and referencing an assembly is not accessing one: loading this assembly does not
    /// arm it, loading a user assembly that derives from <c>Ui.Element</c> does not arm it, and
    /// neither does the reflection discovery itself performs — <c>GetTypes()</c> and resolving
    /// the base type both leave it unarmed. Only constructing a derived instance does, and that
    /// cannot be what arms discovery: the engine constructs one only through a factory that
    /// exists only once discovery has registered the tag, so waiting for it would be a cycle
    /// that never starts.
    /// </para>
    /// <para>
    /// <c>HotReloadManager.EnsureSharedAssembliesInDefault</c> therefore runs the module
    /// constructors of the shared assemblies it loads, which arms this before any user assembly
    /// is loaded. That call is load-bearing, not defensive — without it C#-defined element types
    /// are never discovered at all.
    /// </para>
    /// <para>
    /// Arming late is deliberately harmless: <see cref="UiElementTypes.Arm"/> scans the
    /// already-loaded assemblies as well as subscribing, so whichever of (arm, publish) happens
    /// first, the other covers it. The event is not replayed to late subscribers, which is the
    /// same hazard <c>EditorMenuBridge</c> compensates for with an unconditional refresh at
    /// registration.
    /// </para>
    /// </summary>
    internal static class UiElementTypesBootstrap
    {
        // CA2255 asks for confirmation that this is not a library reaching for a hook it should
        // have exposed as an API instead. It is the opposite: the arming has to happen without
        // the host calling anything, because the host has no reason to know this feature exists.
#pragma warning disable CA2255
        [ModuleInitializer]
#pragma warning restore CA2255
        internal static void Initialize()
        {
            // Nothing may throw out of a module initializer: the CLR turns it into a
            // TypeInitializationException on whatever unrelated type is first touched, which
            // would present as this assembly being unloadable rather than as UI types being
            // unavailable. Arm swallows internally too; this is the outer belt.
            try
            {
                UiElementTypes.Arm();
            }
            catch
            {
            }
        }
    }
}
