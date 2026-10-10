using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace GameEngine.CompileServerSmoke;

internal static class Program
{
    static int Main(string[] args)
    {
        string pipeName = args.Length > 0 ? args[0] : "GE_CompileServer_smoke";
        try
        {
            // Start server
            string repoRoot = TryFindRepoRoot();
            string hostDll = Path.Combine(repoRoot, "Managed", "CompileServerHost", "bin", "Debug", "net10.0", "GameEngine.CompileServerHost.dll");
            if (!File.Exists(hostDll))
            {
                Console.Error.WriteLine($"Host not found: {hostDll}. Build it first.");
                return 2;
            }

            var psi = new ProcessStartInfo("dotnet", $"\"{hostDll}\" {pipeName}")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
            };
            using var p = Process.Start(psi)!;

            // Connect client
            using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            var sw = Stopwatch.StartNew();
            while (!client.IsConnected)
            {
                try { client.Connect(200); } catch { }
                if (sw.Elapsed > TimeSpan.FromSeconds(5))
                {
                    Console.Error.WriteLine("Timed out connecting to pipe");
                    return 3;
                }
            }

            using var writer = new StreamWriter(client, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
            using var reader = new StreamReader(client, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);

            string request = "{\"ProjectRoot\":\".\",\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[],\"PreferredStrategy\":\"Incremental\",\"ForceFull\":false}";
            writer.WriteLine(request);
            string? response = reader.ReadLine();
            if (string.IsNullOrWhiteSpace(response)) { Console.Error.WriteLine("Empty response"); return 4; }
            using var doc = JsonDocument.Parse(response);
            if (!doc.RootElement.TryGetProperty("Success", out var success) || success.GetBoolean() != true)
            {
                Console.Error.WriteLine($"Server reported failure: {response}");
                return 5;
            }
            Console.WriteLine("Smoke OK");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.ToString());
            return 1;
        }
    }

    static string TryFindRepoRoot()
    {
        var cur = Directory.GetCurrentDirectory();
        for (int i = 0; i < 6; i++)
        {
            string probe = Path.Combine(cur, "Managed");
            if (Directory.Exists(probe)) return cur;
            var parent = Directory.GetParent(cur);
            if (parent == null) break;
            cur = parent.FullName;
        }
        return Directory.GetCurrentDirectory();
    }
}

