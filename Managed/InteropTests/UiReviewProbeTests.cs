#pragma warning disable CS1591
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
    /// Review probes for PR #1083 (adversarial review lane, not part of the branch).
    /// Each one attacks a property the branch claims but does not arm.
    /// </summary>
    public class UiReviewProbeTests
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
        private const int kGcAttempts = 30;
        private const int kGcPauseMs = 50;

        [SetUp]
        public void Reset() => Assert.That(GE_TestUI_Reset(), Is.EqualTo(0));

        // ---------------------------------------------------------------------------
        // PROBE 0: WHICH GameEngine.Native library is loaded. The shim is copied under the
        // platform file name in the test output dir while GE_NATIVE_DIR/PATH point at the
        // build tree's real one, so "the tests were green" says nothing until this is pinned.
        // ---------------------------------------------------------------------------

        [Test]
        public void ReportWhichNativeModuleIsLoaded()
        {
            AddElement("probe0");
            string? path = null;
            foreach (System.Diagnostics.ProcessModule m in
                     System.Diagnostics.Process.GetCurrentProcess().Modules)
            {
                if (m.ModuleName != null && IsNativeLibraryFileName(m.ModuleName))
                {
                    path = m.FileName;
                    break;
                }
            }
            TestContext.Out.WriteLine("LOADED GameEngine.Native = " + (path ?? "<not loaded>"));
            Assert.That(path, Is.Not.Null, "GameEngine.Native is not in the module list at all");
            Assert.That(path!.Replace('\\', '/'), Does.Contain("InteropTests/bin"),
                "the REAL engine native DLL won the probe, not the shim double — every UI shim "
                + "assertion in this suite would then be measuring something else");
        }

        // The file names CMake gives GameEngine.Native on Windows, macOS and Linux.
        private static bool IsNativeLibraryFileName(string moduleName) =>
            moduleName.Equals("GameEngine.Native.dll", StringComparison.OrdinalIgnoreCase)
            || moduleName == "libGameEngine.Native.dylib"
            || moduleName == "libGameEngine.Native.so";

        private static ulong AddElement(string elementId, string tag = "uielement")
        {
            Assert.That(GE_TestUI_AddElement(elementId, tag, out ulong instanceId), Is.EqualTo(0));
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

        private static void CollectUntilDead(WeakReference weak)
        {
            for (int i = 0; i < kGcAttempts && weak.IsAlive; i++)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
                Thread.Sleep(kGcPauseMs);
            }
        }

        // ---------------------------------------------------------------------------
        // PROBE 1 (attack 3 inverse): TWO collectible contexts on ONE event. Unloading A
        // must not stop B. The branch arms default-context-survives-scripts-unload; the
        // two-collectible case is the one the per-listener design correction is actually
        // about, because BOTH sides are tracked in s_Tracked.
        // ---------------------------------------------------------------------------

        private const string kSourcesA = @"
public static class ProbeScriptsA
{
    public static int Invocations;
    public static void OnFocus() { ++Invocations; }
}";

        private const string kSourcesB = @"
public static class ProbeScriptsB
{
    public static int Invocations;
    public static void OnFocus() { ++Invocations; }
}";

        [Test]
        public void TwoCollectibleContextsOnOneEvent_UnloadingOneLeavesTheOtherFiring()
        {
            ScriptCompileHelper.EnsureHrmLoaded();

            ulong instanceId = AddElement("hp-panel");
            Ui.Element el = Ui.FindElement("hp-panel")!;

            var (domainA, weakAlcA, weakCbA, alcA) = Subscribe(el, kSourcesA, "ProbeScriptsA");
            var (domainB, _, _, alcB) = Subscribe(el, kSourcesB, "ProbeScriptsB");

            Assert.That(ReferenceEquals(alcA, alcB), Is.False,
                "fixture precondition: the two scripts must land in DIFFERENT collectible contexts");
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "fixture precondition: both listeners must share ONE native subscription");

            Assert.That(Dispatch(instanceId), Is.EqualTo(1));
            Assert.That(InvocationsOf(alcB, "ProbeScriptsB"), Is.EqualTo(1));
            Assert.That(InvocationsOf(alcA, "ProbeScriptsA"), Is.EqualTo(1));

            Assert.That(HotReloadManager.UnloadDomain((ulong)domainA), Is.EqualTo(0),
                "domain A unload was refused");

            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "unloading context A tore down the native subscription context B is still on");
            Dispatch(instanceId);
            Assert.That(InvocationsOf(alcB, "ProbeScriptsB"), Is.EqualTo(2),
                "context B's listener stopped firing when context A unloaded");

            CollectUntilDead(weakAlcA);
            Assert.That(weakCbA.IsAlive, Is.False, "context A's listener was not revoked");

            Assert.That(HotReloadManager.UnloadDomain((ulong)domainB), Is.EqualTo(0));
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0),
                "the LAST collectible listener unloading must take the native subscription down");
            GC.KeepAlive(el);
        }

        private static int InvocationsOf(AssemblyLoadContext alc, string typeName)
        {
            foreach (Assembly a in alc.Assemblies)
            {
                Type? t = a.GetType(typeName, throwOnError: false);
                if (t != null)
                    return (int)t.GetField("Invocations")!.GetValue(null)!;
            }
            throw new InvalidOperationException($"{typeName} not found in the context");
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static (int domain, WeakReference weakAlc, WeakReference weakCb, AssemblyLoadContext alc)
            Subscribe(Ui.Element el, string sources, string typeName)
        {
            var (asm, _) = ScriptCompileHelper.CompileCustomSources(
                sources, assemblyName: typeName + "_" + Guid.NewGuid().ToString("N"));
            int domain = HotReloadManager.LoadCompiledAssembly(asm);
            Assert.That(domain, Is.GreaterThan(0));

            Assembly? loaded = null;
            foreach (Assembly a in HotReloadManager.GetActiveUserAssemblies())
            {
                if (a.GetType(typeName, throwOnError: false) != null)
                {
                    loaded = a;
                    break;
                }
            }
            Assert.That(loaded, Is.Not.Null, typeName + " not found in loaded user assemblies");

            var callback = (Action)Delegate.CreateDelegate(
                typeof(Action), loaded!.GetType(typeName)!.GetMethod("OnFocus")!);
            AssemblyLoadContext alc = AssemblyLoadContext.GetLoadContext(loaded)!;
            Assert.That(alc.IsCollectible, Is.True);

            el.FocusGained += callback;
            return (domain, new WeakReference(alc), new WeakReference(callback), alc);
        }

        // ---------------------------------------------------------------------------
        // PROBE 2 (attack 6): a GC between lookup and lookup must not hand back a ghost.
        // The weak table's corpse-drop path is only exercised when the wrapper is
        // actually collected between two Resolves.
        // ---------------------------------------------------------------------------

        [Test]
        public void AfterTheWrapperIsCollected_ALookupMintsAFreshLiveWrapper()
        {
            ulong instanceId = AddElement("ghost");
            WeakReference weak = ResolveAndForget();
            CollectUntilDead(weak);
            Assert.That(weak.IsAlive, Is.False, "fixture precondition: the wrapper must be collected");

            Ui.Element? fresh = Ui.FindElement("ghost");
            Assert.That(fresh, Is.Not.Null, "the id stopped resolving after its wrapper was collected");
            Assert.That(fresh!.IsAlive, Is.True, "the re-minted wrapper does not address the live element");
            Assert.That(fresh.AddClass("x"), Is.True, "the re-minted wrapper cannot write to its element");

            Ui.Element? again = Ui.FindElement("ghost");
            Assert.That(ReferenceEquals(fresh, again), Is.True,
                "identity did not re-establish after a collect-and-remint");
            Assert.That(instanceId, Is.Not.Zero);
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        private static WeakReference ResolveAndForget() => new WeakReference(Ui.FindElement("ghost"));

        // ---------------------------------------------------------------------------
        // PROBE 3 (attack 6, second half): the doc says the wrapper owns nothing —
        // no IDisposable, no finalizer. Pin it, because adding either later would be a
        // silent API-shape change.
        // ---------------------------------------------------------------------------

        [Test]
        public void WrapperTypesHaveNoFinalizerAndNoIDisposable()
        {
            foreach (Type t in new[] { typeof(Ui.Element), typeof(Ui.Button), typeof(Ui.Label) })
            {
                Assert.That(typeof(IDisposable).IsAssignableFrom(t), Is.False,
                    t.Name + " became IDisposable — the doc says the wrapper owns nothing native");
                MethodInfo? finalizer = t.GetMethod("Finalize",
                    BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.DeclaredOnly);
                Assert.That(finalizer, Is.Null,
                    t.Name + " declares a finalizer — every wrapper would then cost a finalization queue entry");
            }
        }

        // ---------------------------------------------------------------------------
        // PROBE 4, now a regression lock. As first written this arm REPRODUCED a defect:
        // event slots were per-WRAPPER fields while identity is per-ELEMENT, so a subclass
        // claiming an element orphaned the previous wrapper's listeners — -= a silent
        // no-op, += a second native entry for one event. Subscriptions are now keyed by
        // element, so both wrappers share the slot, and the assertions below are inverted
        // from what they were when they found it.
        // ---------------------------------------------------------------------------

        private sealed class Bar : Ui.Element
        {
            public Bar(Ui.Element source) : base(source) { }
        }

        [Test]
        public void SubclassShares_RatherThanOrphans_ThePreviousWrappersSubscription()
        {
            ulong instanceId = AddElement("hp-panel");
            Ui.Element first = Ui.FindElement("hp-panel")!;

            int fired = 0;
            Action listener = () => ++fired;
            first.FocusGained += listener;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1));

            var bar = new Bar(first);
            Assert.That(Ui.FindElement("hp-panel"), Is.SameAs(bar),
                "fixture precondition: the subclass must own the identity");

            // A += through the identity-owning wrapper JOINS the element's subscription.
            int otherFired = 0;
            Action other = () => ++otherFired;
            bar.FocusGained += other;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "the subclass started a second native entry for one event on one element");
            Dispatch(instanceId);
            Assert.That(fired, Is.EqualTo(1));
            Assert.That(otherFired, Is.EqualTo(1));

            // And a -= through it REACHES the listener the other wrapper added.
            bar.FocusGained -= listener;
            Dispatch(instanceId);
            Assert.That(fired, Is.EqualTo(1),
                "unsubscribing through the identity-owning wrapper did not reach a listener the "
                + "previous wrapper added — it is orphaned, and unreachable through the API");
            Assert.That(otherFired, Is.EqualTo(2));

            bar.FocusGained -= other;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0));
            GC.KeepAlive(first);
        }

        // ---------------------------------------------------------------------------
        // PROBE 5, now a regression lock. As first written this arm REPRODUCED the second
        // half of the same defect: the wrapper is held WEAKLY while its subscription is
        // rooted by the GCHandle, so dropping the wrapper stranded the entry — the next
        // lookup minted empty slots, a re-subscribe doubled the native entries, and the
        // first was unreachable through the API forever. Element-keyed slots make the
        // re-minted wrapper find the existing subscription instead.
        // ---------------------------------------------------------------------------

        [Test]
        public void ARemintedWrapperFindsTheElementsExistingSubscription()
        {
            ulong instanceId = AddElement("hp-panel");
            WeakReference weakWrapper = SubscribeAndForgetWrapper();

            CollectUntilDead(weakWrapper);
            Assert.That(weakWrapper.IsAlive, Is.False,
                "fixture precondition: the wrapper must be collectible with a live subscription on it "
                + "(if it is NOT, the subscription roots the wrapper and this hazard does not exist)");

            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "fixture precondition: the native subscription must outlive its wrapper — the GCHandle "
                + "roots the SUBSCRIPTION, not the wrapper");

            Ui.Element fresh = Ui.FindElement("hp-panel")!;
            Action second = () => { };
            fresh.FocusGained += second;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "a re-minted wrapper started a SECOND native entry for the same event");

            fresh.FocusGained -= second;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(1),
                "removing one of two listeners tore the shared subscription down");
            // The listener the COLLECTED wrapper added is still reachable, which is the half
            // that was unreachable forever before element-keyed slots.
            fresh.FocusGained -= StaticListener;
            Assert.That(SubscriptionCount(instanceId), Is.EqualTo(0),
                "the listener the collected wrapper added cannot be removed through the API");
            GC.KeepAlive(fresh);
        }

        // The listener must NOT capture the wrapper, or the subscription's GCHandle would
        // root it and the wrapper could never be collected.
        [MethodImpl(MethodImplOptions.NoInlining)]
        private static WeakReference SubscribeAndForgetWrapper()
        {
            Ui.Element el = Ui.FindElement("hp-panel")!;
            el.FocusGained += StaticListener;
            return new WeakReference(el);
        }

        private static void StaticListener() { }
    }
}
