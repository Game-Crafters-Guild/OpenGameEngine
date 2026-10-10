using System;
using System.Text;

namespace GameEngine.Interop
{
    /// <summary>
    /// The engine's 64-bit string ids on the managed side: FNV-1a over the text's UTF-8 bytes, the hash native
    /// <c>HashStringId</c> (Types/StringId.h) and <c>Input::HashInput</c> compute, so an id made here matches the one
    /// native code makes for the same text. A lone surrogate hashes as U+FFFD, the character UTF-8 marshalling sends
    /// in its place. Allocates nothing.
    /// </summary>
    internal static class StringIds
    {
        private const ulong kFnvOffsetBasis = 14695981039346656037ul;
        private const ulong kFnvPrime = 1099511628211ul;
        private const int kMaxUtf8BytesPerRune = 4;

        public static ulong Hash(string text)
        {
            ArgumentNullException.ThrowIfNull(text);
            Span<byte> utf8 = stackalloc byte[kMaxUtf8BytesPerRune];
            ulong hash = kFnvOffsetBasis;
            foreach (Rune rune in text.EnumerateRunes())
            {
                int length = rune.EncodeToUtf8(utf8);
                for (int i = 0; i < length; ++i)
                {
                    hash ^= utf8[i];
                    hash *= kFnvPrime;
                }
            }
            return hash;
        }
    }
}
