using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;

namespace GameEngine.CoreBridge;

internal sealed class EngineLogWriter : TextWriter
{
    private static TextWriter? s_prevOut;
    private static TextWriter? s_prevErr;
    [ThreadStatic] private static bool s_inLog;
    private static bool s_reportedFallback;

    // Optional lightweight trace for debugging Console redirection and log routing.
    // Enabled only when GE_COREBRIDGE_LOGWRITER_TRACE_FILE is set; writes directly
    // to the specified file and never uses Console or EngineLogWriter again to
    // avoid recursion.
    private static readonly string? s_tracePath = Environment.GetEnvironmentVariable("GE_COREBRIDGE_LOGWRITER_TRACE_FILE");

    private static void Trace(string message)
    {
        if (string.IsNullOrEmpty(s_tracePath))
            return;
        try
        {
            string line = DateTime.Now.ToString("HH:mm:ss.fff ") + message + Environment.NewLine;
            File.AppendAllText(s_tracePath, line);
        }
        catch
        {
            // Never throw from tracing; this is best-effort only.
        }
    }

    // Native fallback for logging when no registered EngineInstanceContext binding is available.
    // This uses the typed C ABI (GE_Log) directly and is only used when no engine instance is
    // registered or its binding has no Log entry. It ensures InitializeOnLoad and other Console.WriteLine
    // calls still reach the native logger instead of disappearing into Debug-only output.
    private static class NativeFallback
    {
        [DllImport("GameEngine.Native", CallingConvention = CallingConvention.Cdecl)]
        internal static extern unsafe int GE_Log(int level, nint msg, uint len);
    }

    internal static void Install(bool redirectError = true)
    {
        if (s_prevOut != null)
        {
            Trace("Install: already installed; skipping.");
            return; // already installed
        }
        try
        {
            var beforeType = Console.Out?.GetType().FullName ?? "<null>";
            s_prevOut = Console.Out;
            Console.SetOut(new EngineLogWriter());
            var afterType = Console.Out?.GetType().FullName ?? "<null>";
            Trace($"Install: redirecting Console.Out {beforeType} -> {afterType}, redirectError={redirectError}");
        }
        catch (Exception ex)
        {
            Trace($"Install: failed to redirect Console.Out: {ex.GetType().Name}: {ex.Message}");
        }
        if (redirectError)
        {
            try
            {
                var beforeErr = Console.Error?.GetType().FullName ?? "<null>";
                s_prevErr = Console.Error;
                Console.SetError(new EngineLogWriter());
                var afterErr = Console.Error?.GetType().FullName ?? "<null>";
                Trace($"Install: redirecting Console.Error {beforeErr} -> {afterErr}");
            }
            catch (Exception ex)
            {
                Trace($"Install: failed to redirect Console.Error: {ex.GetType().Name}: {ex.Message}");
            }
        }
    }

    internal static void Uninstall()
    {
        try { if (s_prevOut != null) { Console.SetOut(s_prevOut); } } catch { }
        try { if (s_prevErr != null) { Console.SetError(s_prevErr); } } catch { }
        s_prevOut = null; s_prevErr = null;
    }

    public override Encoding Encoding => Encoding.UTF8;

    private static void LogInternal(int level, string message)
    {
        try
        {
            if (string.IsNullOrEmpty(message)) return;

            Trace($"LogInternal: level={level} chars={message.Length}");

            // Prevent recursion when Console is redirected to this writer and we log during binding load
            if (s_inLog) { try { if (s_prevOut != null) s_prevOut.Write(message); else System.Diagnostics.Debug.Write(message); } catch { } return; }
            s_inLog = true;
            try
            {
                // Prefer routing through the managed binding (no DllImport) if available
                try
                {
                    var ctx = CoreBridge.GetEngineInstance();
                    if (ctx != null)
                    {
                        var binding = ctx.Binding; // lazy-loads on first access
                        var log = binding.Log;
                        if (log != null)
                        {
                            Trace("LogInternal: routing via the engine instance's binding.Log");
                            var utf8 = System.Text.Encoding.UTF8;
                            int byteLen = utf8.GetByteCount(message);
                            byte[] rented = System.Buffers.ArrayPool<byte>.Shared.Rent(byteLen);
                            try
                            {
                                int written = utf8.GetBytes(message, 0, message.Length, rented, 0);
                                unsafe
                                {
                                    fixed (byte* p = rented)
                                    {
                                        _ = log(level, (nint)p, (uint)written);
                                    }
                                }
                            }
                            finally
                            {
                                System.Buffers.ArrayPool<byte>.Shared.Return(rented);
                            }
                            return;
                        }
                    }
                }
                catch
                {
                    // ignore and try native fallback below
                }

                // Native fallback: route directly through GE_Log so messages are not lost
                // even if the engine instance/binding is missing. This uses the same
                // typed C ABI that tests and tools rely on.
                try
                {
                    Trace("LogInternal: routing via NativeFallback.GE_Log");
                    var utf8 = System.Text.Encoding.UTF8;
                    int byteLen = utf8.GetByteCount(message);
                    byte[] rented = System.Buffers.ArrayPool<byte>.Shared.Rent(byteLen);
                    try
                    {
                        int written = utf8.GetBytes(message, 0, message.Length, rented, 0);
                        unsafe
                        {
                            fixed (byte* p = rented)
                            {
                                _ = NativeFallback.GE_Log(level, (nint)p, (uint)written);
                            }
                        }
                    }
                    finally
                    {
                        System.Buffers.ArrayPool<byte>.Shared.Return(rented);
                    }

                    // Emit a one-time diagnostic into the native log so invalid routing
                    // is visible without relying on Debug-only sinks.
                    if (!s_reportedFallback)
                    {
                        s_reportedFallback = true;
                        const int kWarnLevel = 3; // GE_Log_Warn
                            string diag = "[CoreBridge] EngineLogWriter: using direct GE_Log fallback (no engine instance binding). " +
                                         "Check CoreBridge engine instance registration if this persists.";
                        int diagLen = utf8.GetByteCount(diag);
                        byte[] diagBytes = System.Buffers.ArrayPool<byte>.Shared.Rent(diagLen);
                        try
                        {
                            int diagWritten = utf8.GetBytes(diag, 0, diag.Length, diagBytes, 0);
                            unsafe
                            {
                                fixed (byte* pDiag = diagBytes)
                                {
                                    _ = NativeFallback.GE_Log(kWarnLevel, (nint)pDiag, (uint)diagWritten);
                                }
                            }
                        }
                        finally
                        {
                            System.Buffers.ArrayPool<byte>.Shared.Return(diagBytes);
                        }
                    }
                    return;
                }
                catch
                {
                    // Final fallback: write to Debug only to avoid any risk of recursion
                    try { System.Diagnostics.Debug.Write(message); } catch { }
                }
            }
            finally { s_inLog = false; }
        }
        catch { /* do not throw across TextWriter */ }
    }

    public override void Write(string? value)
    {
        if (string.IsNullOrEmpty(value)) return;
        LogInternal(2, value); // Info by default
    }

    public override void WriteLine(string? value)
    {
        if (string.IsNullOrEmpty(value)) return;
        LogInternal(2, value + Environment.NewLine); // ensure newline
    }

    public override void Write(char value)
    {
        LogInternal(2, value.ToString());
    }

    public override void Write(char[]? buffer, int index, int count)
    {
        if (buffer == null || count <= 0) return;
        LogInternal(2, new string(buffer, index, count));
    }
}

