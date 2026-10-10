using System;
using System.Runtime.InteropServices;
using System.Text;

namespace GameEngine.Scripting
{
    /// <summary>
    /// What a model asset kept from its source file for a game to interpret. The engine reads none of it.
    /// Call from the main thread, as a system's update does: a hot reload replaces a model's extras there,
    /// and a call from another thread is refused.
    /// </summary>
    public static class ModelApi
    {
        /// <summary>
        /// The extras JSON text of the object of <paramref name="kind"/> named <paramref name="name"/> in the
        /// loaded model <paramref name="modelGuid"/>, as the source file writes it: the file's own text for an
        /// extras object, array, number or literal, while an extras value that is a JSON string reads without
        /// its quotes. The bytes are decoded as UTF-8, so an invalid sequence in the file reads as U+FFFD. An
        /// unnamed object is named by the empty string; when two objects of one kind share a name, the first in
        /// the file is read.
        /// </summary>
        /// <param name="modelGuid">
        /// The model asset's GUID; <c>new Guid(text)</c> of the text form scene files write reads the same asset.
        /// </param>
        /// <param name="kind">The kind of object: scene, node, mesh, material or animation.</param>
        /// <param name="name">The object's name in the source file.</param>
        /// <returns>
        /// The text, or an empty string when the model kept no extras for the object: it has none, no
        /// object of that kind has the name, or the import did not keep them (over 256 KiB for the object,
        /// or past 1 MiB for the model, counting each object's name with its extras; the import log names each).
        /// </returns>
        /// <exception cref="ArgumentOutOfRangeException"><paramref name="kind"/> is not a <see cref="ModelObjectKind"/>.</exception>
        /// <exception cref="InvalidOperationException">
        /// No loaded model has <paramref name="modelGuid"/>, or the call came from a thread other than the main thread.
        /// </exception>
        public static string GetExtras(Guid modelGuid, ModelObjectKind kind, string name)
        {
            if (!Enum.IsDefined(kind))
                throw new ArgumentOutOfRangeException(nameof(kind), kind,
                    "ModelApi.GetExtras reads a scene, node, mesh, material or animation (ModelObjectKind).");
            // The engine's GUID bytes are in the order its text form writes them.
            byte[] guidBytes = modelGuid.ToByteArray(bigEndian: true);
            byte[] nameUtf8 = string.IsNullOrEmpty(name) ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(name);
            int rc = Native.GetExtras(guidBytes, (uint)kind, nameUtf8, (uint)nameUtf8.Length, null, 0, out int length);
            ThrowIfFailed(rc, modelGuid);
            if (length == 0)
                return string.Empty;

            byte[] text = new byte[length];
            rc = Native.GetExtras(guidBytes, (uint)kind, nameUtf8, (uint)nameUtf8.Length, text, length, out int written);
            ThrowIfFailed(rc, modelGuid);
            return Encoding.UTF8.GetString(text, 0, Math.Min(written, length));
        }

        private static void ThrowIfFailed(int rc, Guid modelGuid)
        {
            if (rc == (int)NativeResult.NotFound)
                throw new InvalidOperationException(
                    $"ModelApi.GetExtras: no loaded model has the GUID {modelGuid}; a model's extras are readable while it is loaded.");
            if (rc == (int)NativeResult.Fail)
                throw new InvalidOperationException(
                    "ModelApi.GetExtras failed: it reads a model's extras only on the main thread (from a system's update); the engine log names any other cause.");
            if (rc != (int)NativeResult.Ok)
                throw new InvalidOperationException($"ModelApi.GetExtras failed: {(NativeResult)rc}");
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

            [DllImport(DllName, EntryPoint = "GE_Model_GetExtras", CallingConvention = CallingConvention.Cdecl)]
            public static extern int GetExtras(byte[] modelGuidBytes, uint kind, byte[] nameUtf8, uint nameLength,
                                               byte[]? buffer, int bufferLen, out int outLen);
        }
    }
}
