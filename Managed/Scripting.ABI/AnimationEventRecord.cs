using System;
using System.Runtime.InteropServices;
using System.Text;

namespace GameEngine.Scripting
{
    /// <summary>
    /// One animation event an Animator fired, as <see cref="AnimatorApi.PollEvents"/> returns it: the event's name and
    /// its time on the clip. Test the name without allocating by comparing <see cref="NameId"/> with a value from
    /// <see cref="AnimatorApi.EventNameId"/>. Mirrors the native <c>GE_AnimationEventRecord</c> (80 bytes).
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct AnimationEventRecord
    {
        /// <summary>The bytes of the name a record holds. <see cref="NameLength"/> can be larger.</summary>
        public const int NameCapacity = 64;

        /// <summary>The name's id: <see cref="AnimatorApi.EventNameId"/> of the whole name.</summary>
        public ulong NameId;

        /// <summary>The event's time on its clip (or montage), in seconds from the start.</summary>
        public float TimeSeconds;

        /// <summary>The name's length in UTF-8 bytes, whole name.</summary>
        public int NameLength;

        private fixed byte m_NameUtf8[NameCapacity];

        /// <summary>
        /// The event's name. Allocates a string on each read. A name longer than <see cref="NameCapacity"/> bytes
        /// reads as its first <see cref="NameCapacity"/> bytes; <see cref="NameId"/> is always the whole name's.
        /// </summary>
        public string Name
        {
            get
            {
                fixed (byte* name = m_NameUtf8)
                    return Encoding.UTF8.GetString(name, Math.Min(NameLength, NameCapacity));
            }
        }
    }
}
