using System;
using System.Runtime.InteropServices;
using System.Text;
using GameEngine.ECS;
using GameEngine.Interop;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Clip and pose-graph playback on an entity's Animator. Force-state names:
    /// <see cref="PlayState"/> (hard cut) and <see cref="CrossFadeSeconds"/>
    /// (wall-clock seconds, not normalized to the clip's length).
    /// Graph parameters are StringId-hashed names; getters never invent a 0 default.
    /// The events playback fires arrive through <see cref="PollEvents"/>.
    /// </summary>
    public static class AnimatorApi
    {
        public static void PlayState(WorldHandle world, uint entityId, string animationName)
        {
            byte[] utf8 = Encode(animationName);
            int rc = Native.PlayStateOnEntity(world.Handle, entityId, utf8, (uint)utf8.Length);
            ThrowIfFailed(rc, "PlayState");
        }

        public static void PlayState(WorldHandle world, uint entityId, Guid clipGuid)
        {
            int rc = Native.PlayStateClipOnEntity(world.Handle, entityId, clipGuid.ToByteArray());
            ThrowIfFailed(rc, "PlayState");
        }

        public static void CrossFadeSeconds(WorldHandle world, uint entityId, string animationName, float blendSeconds)
        {
            byte[] utf8 = Encode(animationName);
            int rc = Native.CrossFadeSecondsOnEntity(world.Handle, entityId, utf8, (uint)utf8.Length, blendSeconds);
            ThrowIfFailed(rc, "CrossFadeSeconds");
        }

        public static void CrossFadeSeconds(WorldHandle world, uint entityId, Guid clipGuid, float blendSeconds)
        {
            int rc = Native.CrossFadeSecondsClipOnEntity(world.Handle, entityId, clipGuid.ToByteArray(), blendSeconds);
            ThrowIfFailed(rc, "CrossFadeSeconds");
        }

        public static void Pause(WorldHandle world, uint entityId)
        {
            ThrowIfFailed(Native.PauseOnEntity(world.Handle, entityId), "Pause");
        }

        public static void Stop(WorldHandle world, uint entityId)
        {
            ThrowIfFailed(Native.StopOnEntity(world.Handle, entityId), "Stop");
        }

        public static void Seek(WorldHandle world, uint entityId, float timeSeconds)
        {
            ThrowIfFailed(Native.SeekOnEntity(world.Handle, entityId, timeSeconds), "Seek");
        }

        public static void PlayGraph(WorldHandle world, uint entityId, Guid graphGuid)
        {
            int rc = Native.PlayGraphOnEntity(world.Handle, entityId, graphGuid.ToByteArray());
            ThrowIfFailed(rc, "PlayGraph");
        }

        public static void SetFloat(WorldHandle world, uint entityId, string name, float value)
        {
            byte[] utf8 = Encode(name);
            int rc = Native.SetFloatOnEntity(world.Handle, entityId, utf8, (uint)utf8.Length, value);
            ThrowIfFailed(rc, "SetFloat");
        }

        public static void SetBool(WorldHandle world, uint entityId, string name, bool value)
        {
            byte[] utf8 = Encode(name);
            int rc = Native.SetBoolOnEntity(world.Handle, entityId, utf8, (uint)utf8.Length, value ? 1 : 0);
            ThrowIfFailed(rc, "SetBool");
        }

        public static void SetTrigger(WorldHandle world, uint entityId, string name)
        {
            byte[] utf8 = Encode(name);
            int rc = Native.SetTriggerOnEntity(world.Handle, entityId, utf8, (uint)utf8.Length);
            ThrowIfFailed(rc, "SetTrigger");
        }

        /// <summary>
        /// Copies into <paramref name="buffer"/> the animation events the entity's Animator fired in the last
        /// animation wave that no earlier call returned, in the order they fired, and returns how many. A buffer that
        /// fills leaves the rest for the next call. Each event is returned once, and the next animation wave replaces
        /// them with its own, so poll every frame an event matters. Allocates nothing. Call from the main thread, as
        /// a system's update does.
        /// </summary>
        /// <exception cref="InvalidOperationException">
        /// The entity is not alive or has no Animator, or the call came from a thread other than the main thread.
        /// </exception>
        public static int PollEvents(WorldHandle world, uint entityId, Span<AnimationEventRecord> buffer)
        {
            int count;
            int rc;
            unsafe
            {
                fixed (AnimationEventRecord* records = buffer)
                    rc = Native.PollEventsOnEntity(world.Handle, entityId, records, buffer.Length, out count);
            }
            if (rc == (int)NativeResult.Fail)
                throw new InvalidOperationException(
                    "Animator PollEvents failed: it reads an Animator's events only on the main thread (from a system's update); the engine log names any other cause.");
            ThrowIfFailed(rc, "PollEvents");
            return count;
        }

        /// <summary>
        /// The id <see cref="AnimationEventRecord.NameId"/> holds for an event named <paramref name="name"/>: the
        /// engine's StringId of the name, 64-bit FNV-1a over its UTF-8 bytes. Compute it once and keep it.
        /// </summary>
        public static ulong EventNameId(string name) => StringIds.Hash(name);

        private static byte[] Encode(string value) =>
            string.IsNullOrEmpty(value) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(value);

        private static void ThrowIfFailed(int rc, string op)
        {
            if (rc != 0)
                throw new InvalidOperationException($"Animator {op} failed: {(NativeResult)rc}");
        }

        private enum NativeResult
        {
            Ok = 0,
            Fail = -1,
            InvalidArg = -2,
            NotFound = -3,
            NotInitialized = -4
        }

        private static class Native
        {
            private const string DllName = "GameEngine.Native";

            [DllImport(DllName, EntryPoint = "GE_Animator_PlayStateOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int PlayStateOnEntity(ulong worldHandle, ulong entityId, byte[] animationNameUtf8, uint animationNameLength);

            [DllImport(DllName, EntryPoint = "GE_Animator_PlayStateClipOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int PlayStateClipOnEntity(ulong worldHandle, ulong entityId, byte[] clipGuidBytes);

            [DllImport(DllName, EntryPoint = "GE_Animator_CrossFadeSecondsOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int CrossFadeSecondsOnEntity(ulong worldHandle, ulong entityId, byte[] animationNameUtf8, uint animationNameLength, float blendSeconds);

            [DllImport(DllName, EntryPoint = "GE_Animator_CrossFadeSecondsClipOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int CrossFadeSecondsClipOnEntity(ulong worldHandle, ulong entityId, byte[] clipGuidBytes, float blendSeconds);

            [DllImport(DllName, EntryPoint = "GE_Animator_PauseOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int PauseOnEntity(ulong worldHandle, ulong entityId);

            [DllImport(DllName, EntryPoint = "GE_Animator_StopOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int StopOnEntity(ulong worldHandle, ulong entityId);

            [DllImport(DllName, EntryPoint = "GE_Animator_SeekOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int SeekOnEntity(ulong worldHandle, ulong entityId, float timeSeconds);

            [DllImport(DllName, EntryPoint = "GE_Animator_PlayGraphOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int PlayGraphOnEntity(ulong worldHandle, ulong entityId, byte[] graphGuidBytes);

            [DllImport(DllName, EntryPoint = "GE_Animator_SetFloatOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int SetFloatOnEntity(ulong worldHandle, ulong entityId, byte[] nameUtf8, uint nameLength, float value);

            [DllImport(DllName, EntryPoint = "GE_Animator_SetBoolOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int SetBoolOnEntity(ulong worldHandle, ulong entityId, byte[] nameUtf8, uint nameLength, int value);

            [DllImport(DllName, EntryPoint = "GE_Animator_SetTriggerOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int SetTriggerOnEntity(ulong worldHandle, ulong entityId, byte[] nameUtf8, uint nameLength);

            [DllImport(DllName, EntryPoint = "GE_Animator_PollEventsOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern unsafe int PollEventsOnEntity(ulong worldHandle, ulong entityId, AnimationEventRecord* records, int recordCapacity, out int count);
        }
    }
}
