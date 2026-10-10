using System;
using System.Runtime.InteropServices;
using System.Text;
using GameEngine.ECS;

namespace GameEngine.Scripting
{
    [StructLayout(LayoutKind.Sequential)]
    public struct TimelineEvaluateOptions
    {
        public int Loop;
        public float DurationOverride;
    }

    /// <summary>
    /// Managed API for <see cref="GameEngine.Animation.Timeline"/> evaluation and playback control.
    /// Mirrors native <c>EvaluateTimeline</c> and <c>TimelinePlaybackSystem</c>.
    /// </summary>
    public static class TimelineApi
    {
        public static string EvaluateDocument(string timelineJson,
                                              float previousTime,
                                              float currentTime,
                                              bool loop = false,
                                              float durationOverride = 0.0f)
        {
            byte[] utf8 = Encoding.UTF8.GetBytes(timelineJson);
            return EvaluateDocumentUtf8(utf8, previousTime, currentTime, loop, durationOverride);
        }

        public static string EvaluateDocumentUtf8(ReadOnlySpan<byte> timelineJsonUtf8,
                                                    float previousTime,
                                                    float currentTime,
                                                    bool loop = false,
                                                    float durationOverride = 0.0f)
        {
            int required = 4096;
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                byte[] buffer = new byte[required];
                unsafe
                {
                    fixed (byte* jsonPtr = timelineJsonUtf8)
                    fixed (byte* outPtr = buffer)
                    {
                        var options = new TimelineEvaluateOptions
                        {
                            Loop = loop ? 1 : 0,
                            DurationOverride = durationOverride
                        };
                        int written = Native.EvaluateDocumentUtf8(
                            jsonPtr,
                            (uint)timelineJsonUtf8.Length,
                            previousTime,
                            currentTime,
                            ref options,
                            outPtr,
                            (uint)buffer.Length);
                        if (written == (int)NativeResult.BufferFull)
                        {
                            required *= 2;
                            continue;
                        }
                        if (written < 0)
                            throw new InvalidOperationException($"Timeline evaluation failed: {(NativeResult)written}");
                        return Encoding.UTF8.GetString(buffer, 0, written);
                    }
                }
            }

            throw new InvalidOperationException("Timeline evaluation result buffer overflow");
        }

        public static float GetDurationFromDocument(string timelineJson)
        {
            byte[] utf8 = Encoding.UTF8.GetBytes(timelineJson);
            unsafe
            {
                fixed (byte* jsonPtr = utf8)
                {
                    int rc = Native.GetDurationFromDocumentUtf8(jsonPtr, (uint)utf8.Length, out float duration);
                    if (rc != 0)
                        throw new InvalidOperationException("Failed to read timeline duration");
                    return duration;
                }
            }
        }

        public static string EvaluateAsset(Guid timelineAssetGuid,
                                           float previousTime,
                                           float currentTime,
                                           bool loop = false,
                                           float durationOverride = 0.0f)
        {
            byte[] guidBytes = timelineAssetGuid.ToByteArray();
            int required = 4096;
            for (int attempt = 0; attempt < 6; ++attempt)
            {
                byte[] buffer = new byte[required];
                unsafe
                {
                    fixed (byte* outPtr = buffer)
                    {
                        var options = new TimelineEvaluateOptions
                        {
                            Loop = loop ? 1 : 0,
                            DurationOverride = durationOverride
                        };
                        int written = Native.EvaluateAssetGuid(
                            guidBytes,
                            previousTime,
                            currentTime,
                            ref options,
                            outPtr,
                            (uint)buffer.Length);
                        if (written == (int)NativeResult.BufferFull)
                        {
                            required *= 2;
                            continue;
                        }
                        if (written < 0)
                            throw new InvalidOperationException($"Timeline asset evaluation failed: {(NativeResult)written}");
                        return Encoding.UTF8.GetString(buffer, 0, written);
                    }
                }
            }

            throw new InvalidOperationException("Timeline evaluation result buffer overflow");
        }

        public static void Play(WorldHandle world, uint entityId, Guid timelineAssetGuid, bool loop = true, bool autoPlay = true)
        {
            byte[] guidBytes = timelineAssetGuid.ToByteArray();
            int rc = Native.PlayOnEntity(world.Handle, entityId, guidBytes, loop ? 1 : 0, autoPlay ? 1 : 0);
            if (rc != 0)
                throw new InvalidOperationException($"Timeline Play failed: {(NativeResult)rc}");
        }

        public static void Pause(WorldHandle world, uint entityId)
        {
            int rc = Native.PauseOnEntity(world.Handle, entityId);
            if (rc != 0)
                throw new InvalidOperationException($"Timeline Pause failed: {(NativeResult)rc}");
        }

        public static void Stop(WorldHandle world, uint entityId)
        {
            int rc = Native.StopOnEntity(world.Handle, entityId);
            if (rc != 0)
                throw new InvalidOperationException($"Timeline Stop failed: {(NativeResult)rc}");
        }

        public static void Seek(WorldHandle world, uint entityId, float timeSeconds)
        {
            int rc = Native.SeekOnEntity(world.Handle, entityId, timeSeconds);
            if (rc != 0)
                throw new InvalidOperationException($"Timeline Seek failed: {(NativeResult)rc}");
        }

        private static Action<ulong, string, string>? s_MethodListener;
        private static Native.ManagedMethodListenerDelegate? s_MethodListenerThunk;

        public static void SetMethodListener(Action<ulong, string, string>? listener)
        {
            s_MethodListener = listener;
            if (listener == null)
            {
                Native.SetManagedMethodListener(null);
                s_MethodListenerThunk = null;
                return;
            }

            s_MethodListenerThunk ??= OnManagedMethodEvent;
            Native.SetManagedMethodListener(s_MethodListenerThunk);
        }

        private static void OnManagedMethodEvent(ulong entityId,
                                                 IntPtr methodNameUtf8,
                                                 uint methodNameLength,
                                                 IntPtr argumentsUtf8,
                                                 uint argumentsLength)
        {
            if (s_MethodListener == null)
                return;

            string method = ReadUtf8(methodNameUtf8, methodNameLength);
            string args = ReadUtf8(argumentsUtf8, argumentsLength);
            s_MethodListener(entityId, method, args);
        }

        private static string ReadUtf8(IntPtr ptr, uint length)
        {
            if (ptr == IntPtr.Zero || length == 0)
                return string.Empty;
            byte[] bytes = new byte[length];
            Marshal.Copy(ptr, bytes, 0, (int)length);
            return Encoding.UTF8.GetString(bytes);
        }

        private enum NativeResult
        {
            Ok = 0,
            Fail = -1,
            InvalidArg = -2,
            NotFound = -3,
            NotInitialized = -4,
            BufferFull = -6
        }

        private static unsafe class Native
        {
            private const string DllName = "GameEngine.Native";

            [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
            public delegate void ManagedMethodListenerDelegate(ulong entityId,
                                                               IntPtr methodNameUtf8,
                                                               uint methodNameLength,
                                                               IntPtr argumentsUtf8,
                                                               uint argumentsLength);

            [DllImport(DllName, EntryPoint = "GE_Timeline_EvaluateDocumentUtf8", CallingConvention = CallingConvention.Cdecl)]
            public static extern int EvaluateDocumentUtf8(byte* timelineJsonUtf8,
                                                          uint timelineJsonLength,
                                                          float previousTime,
                                                          float currentTime,
                                                          ref TimelineEvaluateOptions options,
                                                          byte* outResultJsonUtf8,
                                                          uint outBufferBytes);

            [DllImport(DllName, EntryPoint = "GE_Timeline_GetDurationFromDocumentUtf8", CallingConvention = CallingConvention.Cdecl)]
            public static extern int GetDurationFromDocumentUtf8(byte* timelineJsonUtf8,
                                                                 uint timelineJsonLength,
                                                                 out float outDurationSeconds);

            [DllImport(DllName, EntryPoint = "GE_Timeline_EvaluateAssetGuid", CallingConvention = CallingConvention.Cdecl)]
            public static extern int EvaluateAssetGuid(byte[] timelineGuidBytes,
                                                         float previousTime,
                                                         float currentTime,
                                                         ref TimelineEvaluateOptions options,
                                                         byte* outResultJsonUtf8,
                                                         uint outBufferBytes);

            [DllImport(DllName, EntryPoint = "GE_Timeline_PlayOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int PlayOnEntity(ulong worldHandle,
                                                  ulong entityId,
                                                  byte[] timelineGuidBytes,
                                                  int loop,
                                                  int autoPlay);

            [DllImport(DllName, EntryPoint = "GE_Timeline_PauseOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int PauseOnEntity(ulong worldHandle, ulong entityId);

            [DllImport(DllName, EntryPoint = "GE_Timeline_StopOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int StopOnEntity(ulong worldHandle, ulong entityId);

            [DllImport(DllName, EntryPoint = "GE_Timeline_SeekOnEntity", CallingConvention = CallingConvention.Cdecl)]
            public static extern int SeekOnEntity(ulong worldHandle, ulong entityId, float timeSeconds);

            [DllImport(DllName, EntryPoint = "GE_Timeline_SetManagedMethodListener", CallingConvention = CallingConvention.Cdecl)]
            public static extern void SetManagedMethodListener(ManagedMethodListenerDelegate? listener);
        }
    }
}
