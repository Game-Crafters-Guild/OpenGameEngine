using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using GameEngine.Scripting;
using NUnit.Framework;
using GameEngine.ManagedInteropTests;

namespace InteropTests
{
    /// <summary>
    /// The managed half of C#-defined element types: what happens to a batch that has been
    /// discovered but not yet registered when its load context goes away underneath it.
    /// <para>
    /// <b>Why this suite exists.</b> Everything else covering element types drives the engine
    /// side — <c>ManagedTypeLifecycleTests</c> against the real registry, the owner arms against
    /// the real exports. The staging/window/unload ordering is pure managed bookkeeping and was
    /// untested, which is exactly where the staged-batch defect lived: a batch outliving its
    /// context, registered later against a dead assembly, holding its tag for the process.
    /// </para>
    /// <para>
    /// <b>The two runtime semantics these arms rest on</b> (both reproduced on a standalone probe
    /// before the fix was designed): <c>Unloading</c> fires SYNCHRONOUSLY inside <c>Unload()</c>,
    /// and subscribing after that point succeeds silently and never runs. So a subscription taken
    /// in the window — one main-thread task after staging — can be dead on arrival, which is why
    /// the owner and the unload subscription are taken at stage time instead.
    /// </para>
    /// <para>
    /// The engine is the <c>GameEngine.NativeShim</c> double, whose registration window is
    /// DEFERRED exactly as the real one is: requesting a window raises a flag and
    /// <c>GE_TestUI_DrainRegistrationWindow</c> runs it. That gap is the interval under test.
    /// </para>
    /// </summary>
    [TestFixture]
    public class UiElementTypeStagingTests
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DrainRegistrationWindow(out int outRan);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_TagOwner(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outOwner);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_TypeOwnerStats(out ulong minted, out ulong live,
                                                           out ulong orphanCalls, out ulong verdictCalls);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_ResetTypeLifecycle();

        private static readonly List<AssemblyLoadContext> s_Contexts = new List<AssemblyLoadContext>();

        /// <summary>Clear the double's type-lifecycle state between arms.</summary>
        [SetUp]
        public void SetUp() => GE_TestUI_ResetTypeLifecycle();

        /// <summary>Unload every context an arm loaded, so none outlives its test.</summary>
        [TearDown]
        public void TearDown()
        {
            foreach (AssemblyLoadContext alc in s_Contexts)
            {
                try { alc.Unload(); } catch { }
            }
            s_Contexts.Clear();
        }

        // A user assembly whose only content is one [UiElement] type, compiled against the real
        // Scripting.ABI so the attribute and the base class are the SAME types the discovery scan
        // compares by full name.
        private static Assembly LoadTypeAssembly(string tag, out AssemblyLoadContext context)
        {
            string abiPath = typeof(Ui.Element).Assembly.Location;
            string source =
                "using GameEngine.Scripting;\n" +
                "[UiElement(\"" + tag + "\")]\n" +
                "public class ProbeElement : Ui.Element { public ProbeElement() { } }\n";

            (byte[] asm, byte[]? pdb) = ScriptCompileHelper.CompileCustomSources(
                source, assemblyName: "ProbeTypes_" + tag,
                extraReferencePaths: new[] { abiPath });

            var alc = new AssemblyLoadContext("probe_" + tag, isCollectible: true);
            s_Contexts.Add(alc);
            context = alc;
            using var peStream = new MemoryStream(asm);
            return alc.LoadFromStream(peStream);
        }

        /// <summary>
        /// A batch staged for a context that unloads before the window runs must not be registered:
        /// the unload that would have released its tags has already happened, so a claim made after it
        /// holds the tag for the rest of the process.
        /// </summary>
        [Test]
        public void AStagedBatchIsPurgedWhenItsContextUnloadsBeforeTheWindowRuns()
        {
            Assembly asm = LoadTypeAssembly("purged-tag", out AssemblyLoadContext alc);

            UiElementTypes.StageTypesFrom(asm, alc);
            GE_TestUI_TypeOwnerStats(out ulong mintedAfterStage, out _, out _, out _);
            Assert.That(mintedAfterStage, Is.EqualTo(1UL),
                "the owner is taken at stage time, where the context is observably alive");

            // The second swap: the context goes before its window is ever drained. Unloading runs
            // synchronously here, which is the only moment the purge can happen.
            alc.Unload();

            GE_TestUI_DrainRegistrationWindow(out int ran);
            Assert.That(ran, Is.EqualTo(1), "the window must actually have run for this to mean anything");

            GE_TestUI_TagOwner("purged-tag", out ulong holder);
            Assert.That(holder, Is.EqualTo(0UL),
                "a batch whose context unloaded must not claim its tag: the unload that would " +
                "release it has already happened, so the claim would hold the tag for the process");

            GE_TestUI_TypeOwnerStats(out ulong minted, out _, out _, out _);
            Assert.That(minted, Is.EqualTo(1UL),
                "and the window must not mint a SECOND owner for an already-dead context");
        }

        /// <summary>The positive control: a live context's batch still claims its tag.</summary>
        [Test]
        public void AStagedBatchWhoseContextIsStillAliveRegistersNormally()
        {
            // The positive control. Without it the arm above would pass just as well against a
            // binding that had stopped registering anything at all.
            Assembly asm = LoadTypeAssembly("live-tag", out AssemblyLoadContext alc);

            UiElementTypes.StageTypesFrom(asm, alc);
            GE_TestUI_DrainRegistrationWindow(out int ran);
            Assert.That(ran, Is.EqualTo(1));

            GE_TestUI_TagOwner("live-tag", out ulong holder);
            Assert.That(holder, Is.Not.EqualTo(0UL), "a live context's batch must claim its tag");

            GE_TestUI_TypeOwnerStats(out ulong minted, out _, out _, out _);
            Assert.That(minted, Is.EqualTo(1UL), "one owner per context, not one per window");
        }

        /// <summary>Unloading frees the tags the context claimed, so a reload can claim them again.</summary>
        [Test]
        public void UnloadingSweepsTheTagsTheContextActuallyClaimed()
        {
            // The other half of the ordering: a batch that DID register must have its tags swept
            // when the context goes, or the next load of the same type is refused its own tag.
            Assembly asm = LoadTypeAssembly("swept-tag", out AssemblyLoadContext alc);

            UiElementTypes.StageTypesFrom(asm, alc);
            GE_TestUI_DrainRegistrationWindow(out _);
            GE_TestUI_TagOwner("swept-tag", out ulong beforeUnload);
            Assert.That(beforeUnload, Is.Not.EqualTo(0UL));

            alc.Unload();

            GE_TestUI_TagOwner("swept-tag", out ulong afterUnload);
            Assert.That(afterUnload, Is.EqualTo(0UL),
                "the unload seam frees the tag, which is what lets the reloaded assembly claim it");
        }

        /// <summary>Re-scanning a context reuses its owner id rather than minting one per scan.</summary>
        [Test]
        public void ARestagedContextReusesItsOwnerRatherThanMintingPerScan()
        {
            // Discovery re-scans every active assembly on every reload event, so staging the same
            // context twice is the ordinary case, not an edge one. A fresh owner per scan would
            // leak ids and split one context's tags across several sweeps.
            Assembly asm = LoadTypeAssembly("restaged-tag", out AssemblyLoadContext alc);

            UiElementTypes.StageTypesFrom(asm, alc);
            UiElementTypes.StageTypesFrom(asm, alc);
            GE_TestUI_DrainRegistrationWindow(out _);

            GE_TestUI_TypeOwnerStats(out ulong minted, out _, out _, out _);
            Assert.That(minted, Is.EqualTo(1UL), "one owner for the context, however often it is scanned");
        }
    }
}
