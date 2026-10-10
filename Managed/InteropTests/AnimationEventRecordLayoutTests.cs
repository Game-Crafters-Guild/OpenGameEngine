using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// AnimationEventRecord mirrors the native GE_AnimationEventRecord (Scripting/AnimatorABI.h) byte for byte:
    /// AnimatorApi.PollEvents hands native code a pointer to an array of them.
    /// </summary>
    public class AnimationEventRecordLayoutTests
    {
        /// <summary>80 bytes, with the name id, time, name length and name bytes at 0, 8, 12 and 16.</summary>
        [Test]
        public void TheRecordHasTheNativeLayout()
        {
            Assert.That(Unsafe.SizeOf<AnimationEventRecord>(), Is.EqualTo(80));
            Assert.That(Marshal.SizeOf<AnimationEventRecord>(), Is.EqualTo(80));
            Assert.That(Marshal.OffsetOf<AnimationEventRecord>(nameof(AnimationEventRecord.NameId)).ToInt32(), Is.EqualTo(0));
            Assert.That(Marshal.OffsetOf<AnimationEventRecord>(nameof(AnimationEventRecord.TimeSeconds)).ToInt32(),
                        Is.EqualTo(8));
            Assert.That(Marshal.OffsetOf<AnimationEventRecord>(nameof(AnimationEventRecord.NameLength)).ToInt32(),
                        Is.EqualTo(12));
            Assert.That(Marshal.OffsetOf<AnimationEventRecord>("m_NameUtf8").ToInt32(), Is.EqualTo(16));
        }
    }
}
