using System;
using System.Text;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A UTF-8 view over a native event payload — "looks like a string, copies when you
        /// keep it", enforced by the compiler rather than by documentation.
        /// <para>
        /// The bytes belong to a dispatch-owned native buffer that is valid for exactly the
        /// handler call the view arrives in, and stays byte-identical for that whole call even
        /// if a handler writes the control mid-dispatch (the engine copies before it
        /// dispatches). Because this is a ref struct, the compiler refuses every construct
        /// that could let it outlive the handler: storing it in a field, capturing it in a
        /// lambda, boxing it, carrying it across an await. What it does NOT do is make every
        /// assignment a copy — another local is still a view. The one visible copy is
        /// <see cref="ToString"/>.
        /// </para>
        /// <para>
        /// Native strings are UTF-8; a C# string is UTF-16. The view is honestly a span of
        /// BYTES, and the transcode is paid only at materialisation — a handler that only
        /// compares or measures never pays it, which is the point: the dispatch path
        /// allocates nothing.
        /// </para>
        /// </summary>
        public readonly ref struct Utf8Text
        {
            // Equals(string) transcodes the other side a chunk at a time, so it allocates
            // nothing however long either side is. UiTextValueEventTests straddles this exact
            // char boundary with a surrogate pair, so the arm proving a pair is never split
            // is written against this number: changing it changes what that arm exercises.
            private const int kEqualsChunkChars = 64;
            // What a chunk can encode to: 64 chars at 3 bytes each, plus 4 for one surrogate
            // pair pulled in whole at the boundary — comfortably inside the buffer.
            private const int kEqualsChunkBytes = 256;

            private readonly ReadOnlySpan<byte> m_Utf8;

            internal Utf8Text(ReadOnlySpan<byte> utf8) => m_Utf8 = utf8;

            /// <summary>The raw UTF-8 bytes, valid for this handler call only.</summary>
            public ReadOnlySpan<byte> Utf8 => m_Utf8;

            /// <summary>Length in UTF-8 BYTES — not chars, not codepoints.</summary>
            public int Utf8Length => m_Utf8.Length;

            public bool IsEmpty => m_Utf8.IsEmpty;

            /// <summary>
            /// Materialise a real string — the one place this type allocates, and the way to
            /// keep the text past the handler.
            /// </summary>
            public override string ToString()
                => m_Utf8.IsEmpty ? string.Empty : Encoding.UTF8.GetString(m_Utf8);

            /// <summary>Byte-wise comparison against UTF-8 bytes. Never allocates.</summary>
            public bool Equals(ReadOnlySpan<byte> utf8) => m_Utf8.SequenceEqual(utf8);

            /// <summary>
            /// True when the view's bytes are exactly UTF-8(<paramref name="other"/>).
            /// Transcodes <paramref name="other"/> in bounded stack chunks, so it never
            /// allocates however long either side is.
            /// </summary>
            public bool Equals(string? other)
            {
                if (other is null)
                    return false;
                // Length gate first: GetByteCount walks the string without allocating, and a
                // mismatch is the common case worth answering immediately.
                if (Encoding.UTF8.GetByteCount(other) != m_Utf8.Length)
                    return false;

                Span<byte> chunk = stackalloc byte[kEqualsChunkBytes];
                ReadOnlySpan<char> rest = other;
                ReadOnlySpan<byte> bytes = m_Utf8;
                while (!rest.IsEmpty)
                {
                    int take = Math.Min(kEqualsChunkChars, rest.Length);
                    // Never split a surrogate pair across chunks: each half would encode as
                    // U+FFFD and the comparison would lie in both directions.
                    if (char.IsHighSurrogate(rest[take - 1]) && take < rest.Length)
                        ++take;
                    int written = Encoding.UTF8.GetBytes(rest[..take], chunk);
                    if (written > bytes.Length || !bytes[..written].SequenceEqual(chunk[..written]))
                        return false;
                    bytes = bytes[written..];
                    rest = rest[take..];
                }
                return bytes.IsEmpty;
            }
        }
    }
}
