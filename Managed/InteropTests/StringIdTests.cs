using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The managed string ids equal the native ones: FNV-1a over the text's UTF-8 bytes, which native HashStringId
    /// (Types/StringId.h) and Input::HashInput compute. The expected values are native HashStringId's for the same text.
    /// </summary>
    public class StringIdTests
    {
        private const ulong kHitId = 0x33732819300680AAul;
        private const ulong kAccentedId = 0xE30DE30E796A47EBul;
        private const string kAccented = "\u00E9p\u00E9";

        /// <summary>An animation event's name id matches the native id for an ASCII and a non-ASCII name.</summary>
        [Test]
        public void EventNameIdMatchesTheNativeStringId()
        {
            Assert.That(AnimatorApi.EventNameId("hit"), Is.EqualTo(kHitId));
            Assert.That(AnimatorApi.EventNameId(kAccented), Is.EqualTo(kAccentedId));
        }

        /// <summary>An input action or context name's id matches the native id for an ASCII and a non-ASCII name.</summary>
        [Test]
        public void HashInputMatchesTheNativeId()
        {
            Assert.That(InputIds.HashInput("hit"), Is.EqualTo(kHitId));
            Assert.That(InputIds.HashInput(kAccented), Is.EqualTo(kAccentedId));
        }
    }
}
