using System;
using System.Runtime.InteropServices;
using System.Text;
using NUnit.Framework;

namespace GameEngine.Tests.Interop
{
    internal static class Native
    {
        private const string kLib = "GameEngine.Native";
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        public static extern unsafe int GE_Log(int level, byte* msg, uint len);
    }

    public class LoggingInteropTests
    {
        [Test]
        public unsafe void GE_Log_Utf8Bytes_Roundtrip_DoesNotCrash()
        {
            string msg = "[InteropTest] こんにちは UTF-8 π 🚀";
            var utf8 = Encoding.UTF8;
            int byteLen = utf8.GetByteCount(msg);
            byte[] buffer = new byte[byteLen];
            int written = utf8.GetBytes(msg, 0, msg.Length, buffer, 0);
            fixed (byte* p = buffer)
            {
                int rc = Native.GE_Log(2 /*Info*/, p, (uint)written);
                Assert.That(rc, Is.EqualTo(0).Or.GreaterThan(0));
            }
        }
    }
}

