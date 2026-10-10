using System.Runtime.CompilerServices;
using NUnit.Framework;
using GameEngine.Scripting;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The managed half of the GE_UIEventData layout pin. The event callback is bound as
    /// <c>delegate* unmanaged[Cdecl]&lt;NativeEventData*, nint, void&gt;</c>, so the runtime reads
    /// the native bytes through this mirror's OWN layout and no marshaller ever reconciles the
    /// two: a field added, reordered or resized on either side re-points every read here
    /// silently, with nothing failing to compile. The other half is the static_assert block
    /// under GE_UIEventData in Engine/Include/Scripting/ScriptingABI.h, holding the same
    /// numbers; the two are edited together or not at all.
    /// <para>
    /// Measured with <see cref="Unsafe"/> rather than <c>Marshal.OffsetOf</c> on purpose.
    /// Marshal reports the MARSHALLED layout, which is not the layout a pointer-bound callback
    /// reads — those two coincide here only because every field is blittable, and a later field
    /// that is not would move them apart while Marshal kept reporting the tidy numbers.
    /// <c>Marshal.OffsetOf</c> cannot state this pin's assertion at all in the case that matters
    /// most: the only layout the runtime is free to reorder is <c>LayoutKind.Auto</c>, and for an
    /// Auto struct Marshal REFUSES to answer — <c>ArgumentException</c>, "cannot be marshaled as
    /// an unmanaged structure; no meaningful size or offset can be computed" — so the reordering
    /// this pin exists to catch is the one measurement Marshal declines to make.
    /// <see cref="Unsafe"/> reports the runtime layout in every case, which is the only layout
    /// the callback ever reads.
    /// </para>
    /// <para>
    /// What neither half of the pin can see is the TAIL PADDING. <c>TextLen</c> ends at 60 and
    /// the struct rounds up to 64, so a field of up to four bytes appended after it lands inside
    /// padding that already existed: the size assertion and every offset below still pass while
    /// the two sides have stopped agreeing about byte 60. An append is pinned by adding its own
    /// assertion here and in the native block, or it is not pinned at all.
    /// </para>
    /// </summary>
    public class UiEventDataLayoutTests
    {
        private static long ByteOffsetOf<T>(ref Ui.NativeEventData origin, ref T field)
            => (long)Unsafe.ByteOffset(
                ref Unsafe.As<Ui.NativeEventData, byte>(ref origin),
                ref Unsafe.As<T, byte>(ref field));

        /// <summary>
        /// Every field, not just the text tail: an offset pin that skipped the head would let a
        /// reorder above the pointer shift nine reads while still landing `text` on 48.
        /// </summary>
        [Test]
        public void NativeEventDataMatchesTheAbiStructLayout()
        {
            Ui.NativeEventData d = default;

            Assert.That(Unsafe.SizeOf<Ui.NativeEventData>(), Is.EqualTo(64),
                        "sizeof(GE_UIEventData) is 64 with 64-bit pointers; the mirror must agree");

            Assert.That(ByteOffsetOf(ref d, ref d.ElementInstanceId), Is.EqualTo(0), "elementInstanceId");
            Assert.That(ByteOffsetOf(ref d, ref d.EventId), Is.EqualTo(8), "eventId");
            Assert.That(ByteOffsetOf(ref d, ref d.X), Is.EqualTo(16), "x");
            Assert.That(ByteOffsetOf(ref d, ref d.Y), Is.EqualTo(20), "y");
            Assert.That(ByteOffsetOf(ref d, ref d.Button), Is.EqualTo(24), "button");
            Assert.That(ByteOffsetOf(ref d, ref d.Mods), Is.EqualTo(28), "mods");
            Assert.That(ByteOffsetOf(ref d, ref d.Value), Is.EqualTo(32), "value");
            Assert.That(ByteOffsetOf(ref d, ref d.ScrollX), Is.EqualTo(36), "scrollX");
            Assert.That(ByteOffsetOf(ref d, ref d.ScrollY), Is.EqualTo(40), "scrollY");

            // The padded tail: scrollY ends at 44 and Text is pointer-aligned, so four implicit
            // bytes sit between them. A mirror that packed them would read the payload pointer
            // out of scrollY's neighbour and hand handlers a wild address.
            Assert.That(ByteOffsetOf(ref d, ref d.Text), Is.EqualTo(48), "text");
            Assert.That(ByteOffsetOf(ref d, ref d.TextLen), Is.EqualTo(56), "textLen");
        }
    }
}
