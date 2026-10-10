using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Threading;
using NUnit.Framework;
using GameEngine.HotReload;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The scripts-unload seam under <see cref="Ui.Element"/>'s events. A subscription roots its
    /// managed listeners with a normal GCHandle so native can invoke them; when a listener
    /// belongs to a collectible scripts context, that root is also a root of the whole context.
    /// These tests pin what the runtime actually does with such a handle across an unload, that
    /// the binding revokes the listeners the unloading context owns, and that it revokes ONLY
    /// those — engine and editor UI register their own handlers on the same elements, and a
    /// listener from another context on the same event must keep working.
    /// <para>
    /// Real subscriptions, driven against the native shim's element test double
    /// (Tests/NativeShim/GameEngineNativeShimUi.cpp): a dotnet-test host has no mounted UI, so
    /// the real exports could only answer not-found. The engine-side behaviour is pinned in
    /// UITests.
    /// </para>
    /// </summary>
    public class UiSubscriptionAlcSeamTests
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_AddElement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string elementId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outInstanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_SubscriptionCount(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? eventName, out int outCount);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Dispatch(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName, float x, float y, int mods,
            out int outInvoked);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Reset();

        private const string kFocusIn = "UI.FocusIn";

        // The script's callback is a plain no-argument method, so it binds to the Action-shaped
        // FocusGained without the compiled assembly needing a reference to this binding.
        private const string kSources = @"
public static class ClickScripts
{
    public static int Invocations;
    public static void OnClick() { ++Invocations; }
}";

        private const int kGcAttempts = 30;
        private const int kGcPauseMs = 50;

        private static int s_DefaultContextInvocations;

        /// <summary>Each test builds its own elements, so the double starts empty.</summary>
        [SetUp]
        public void Reset()
        {
            Assert.That(GE_TestUI_Reset(), Is.EqualTo(0));
            s_DefaultContextInvocations = 0;
        }

        /// <summary>
        /// Characterizes the hazard the revocation exists for: a normal (non-weak) GCHandle over
        /// a delegate whose method lives in a collectible context is a GC root FOR THAT CONTEXT.
        /// The unload is accepted, but nothing is collected and the callback stays fully
        /// invocable — a silent leak that keeps stale script code reachable, not a dead target.
        /// </summary>
        [Test]
        public void StrongGcHandleOnCollectibleDelegate_KeepsContextAliveAndCallbackInvocable()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            var (domain, weakAlc, gch) = LoadAndRootWithGcHandle();
            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0),
                "domain unload was refused");

            CollectUntilDead(weakAlc);
            try
            {
                Assert.That(weakAlc.IsAlive, Is.True,
                    "a strong GCHandle over a collectible-context delegate did NOT pin its ALC — "
                    + "the revocation design assumes it does");
                AssertTargetIsStillInvocable(gch);
            }
            finally
            {
                gch.Free();
            }

            // Freeing the handle is the whole remedy: with the last root gone, the ALC collects.
            CollectUntilDead(weakAlc);
            Assert.That(weakAlc.IsAlive, Is.False,
                "ALC survived even after its only root was freed — something else pins it");
        }

        /// <summary>
        /// The fix: a subscription made from a collectible context and never unsubscribed is
        /// revoked when that context unloads — the native entry goes and the GCHandle is freed,
        /// so the context can actually collect.
        /// </summary>
        [Test]
        public void UndisposedSubscriptionFromCollectibleContext_IsRevokedOnUnload()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            ulong instanceId = AddElement("hp-panel");
            var (domain, weakAlc, weakCallback) = SubscribeFromScriptContext(instanceId);
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "fixture precondition: the script's listener must have created a native subscription");

            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0),
                "domain unload was refused");

            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0),
                "the native subscription outlived the context that owned its only listener");

            CollectUntilDead(weakAlc);
            Assert.That(weakCallback.IsAlive, Is.False,
                "the rooted listener survived the unload — its GCHandle was not freed");
            Assert.That(weakAlc.IsAlive, Is.False,
                "the collectible ALC survived the unload — an unrevoked subscription still pins it");
        }

        /// <summary>
        /// Revocation must reach only the listeners the unloading context owns. Two listeners on
        /// ONE event share one native subscription, so a per-subscription revocation would take
        /// the surviving context's listener down with it — this arm is what forbids that.
        /// </summary>
        [Test]
        public void ListenerFromDefaultContext_SurvivesAScriptsUnload()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            ulong instanceId = AddElement("hp-panel");
            Ui.Element el = Ui.FindElement("hp-panel")!;

            Action defaultContextListener = DefaultContextFocus;
            el.FocusGained += defaultContextListener;

            var (domain, weakAlc, weakScriptCallback) = SubscribeFromScriptContext(instanceId, el);
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "fixture precondition: both listeners must share ONE native subscription");
            Assert.That(Dispatch(instanceId), Is.EqualTo(1));
            Assert.That(s_DefaultContextInvocations, Is.EqualTo(1));

            Assert.That(HotReloadManager.UnloadDomain((ulong)domain), Is.EqualTo(0),
                "domain unload was refused");

            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "a scripts unload tore down a native subscription a default-context listener is "
                + "still on — revocation is per-subscription, and it must be per-listener");
            Dispatch(instanceId);
            Assert.That(s_DefaultContextInvocations, Is.EqualTo(2),
                "the default-context listener stopped firing after a scripts unload");

            CollectUntilDead(weakAlc);
            Assert.That(weakScriptCallback.IsAlive, Is.False,
                "the scripts-context listener was not revoked");
            Assert.That(weakAlc.IsAlive, Is.False,
                "the collectible ALC survived — its listener still pins it");

            el.FocusGained -= defaultContextListener;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0));
            GC.KeepAlive(el);
        }

        private static void DefaultContextFocus() => ++s_DefaultContextInvocations;

        private static ulong AddElement(string elementId)
        {
            Assert.That(GE_TestUI_AddElement(elementId, "uielement", out ulong instanceId), Is.EqualTo(0));
            return instanceId;
        }

        private static int SubscriptionCount(ulong instanceId)
        {
            Assert.That(GE_TestUI_SubscriptionCount(instanceId, kFocusIn, out int count), Is.EqualTo(0));
            return count;
        }

        private static int Dispatch(ulong instanceId)
        {
            Assert.That(GE_TestUI_Dispatch(instanceId, kFocusIn, 0f, 0f, 0, out int invoked), Is.EqualTo(0));
            return invoked;
        }

        /// <summary>
        /// Reads and invokes the rooted callback exactly as the native trampoline does. In its own
        /// NoInlining frame so the strong reference it takes cannot outlive the call and pin the
        /// context the caller is about to assert the collection of.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static void AssertTargetIsStillInvocable(GCHandle gch)
        {
            object? target = gch.Target;
            Assert.That(target, Is.InstanceOf<Action>(),
                "GCHandle.Target after unload was not the rooted Action — the callback did not die, "
                + "it is still fully invocable");
            ((Action)target!)();
        }

        /// <summary>
        /// Loads a script assembly into a fresh collectible domain and roots one of its methods
        /// behind a normal GCHandle, exactly as a subscription does. NoInlining keeps the strong
        /// locals out of the caller's frame so the only remaining root is the handle itself.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static (int domain, WeakReference weakAlc, GCHandle gch) LoadAndRootWithGcHandle()
        {
            var (domain, callback, alc) = LoadScriptCallback();
            return (domain, new WeakReference(alc), GCHandle.Alloc(callback));
        }

        /// <summary>
        /// Subscribes a script-context listener to the element's FocusGained and deliberately
        /// does NOT unsubscribe — that is the case under test. NoInlining keeps the strong
        /// locals out of the caller's frame so the subscription's own root is the only one left.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static (int domain, WeakReference weakAlc, WeakReference weakCallback) SubscribeFromScriptContext(
            ulong instanceId, Ui.Element? element = null)
        {
            var (domain, callback, alc) = LoadScriptCallback();
            Ui.Element el = element ?? Ui.FindElement("hp-panel")!;
            el.FocusGained += callback;
            Assert.That(instanceId, Is.Not.Zero);
            return (domain, new WeakReference(alc), new WeakReference(callback));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static (int domain, Action callback, AssemblyLoadContext alc) LoadScriptCallback()
        {
            var (asm, _) = ScriptCompileHelper.CompileCustomSources(
                kSources, assemblyName: "ClickSeamScripts_" + Guid.NewGuid().ToString("N"));
            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            Assembly? loaded = null;
            foreach (var a in HotReloadManager.GetActiveUserAssemblies())
            {
                if (a.GetType("ClickScripts", throwOnError: false) != null)
                {
                    loaded = a;
                    break;
                }
            }
            Assert.That(loaded, Is.Not.Null, "ClickScripts type not found in loaded user assemblies");

            var callback = (Action)Delegate.CreateDelegate(
                typeof(Action), loaded!.GetType("ClickScripts")!.GetMethod("OnClick")!);

            var alc = AssemblyLoadContext.GetLoadContext(loaded);
            Assert.That(alc, Is.Not.Null);
            Assert.That(alc!.IsCollectible, Is.True, "test assembly did not load into a collectible ALC");
            return (domain, callback, alc);
        }

        private static void CollectUntilDead(WeakReference weak)
        {
            for (int i = 0; i < kGcAttempts && weak.IsAlive; i++)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                Thread.Sleep(kGcPauseMs);
            }
        }
    }
}
