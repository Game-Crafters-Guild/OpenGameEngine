using System;
using System.Text;

namespace GameEngine.CompileServerStub;

internal static class Program
{
    // Very simple stub IPC: read lines from stdin, write fixed success JSON to stdout
    // Protocol (temporary): single-line JSON requests; respond with { "success": true }
    static int Main()
    {
        Console.InputEncoding = Encoding.UTF8;
        Console.OutputEncoding = Encoding.UTF8;
        string? line;
        while ((line = Console.ReadLine()) != null)
        {
            if (line.Length == 0) continue;
            Console.WriteLine("{\"success\":true,\"warnings\":[],\"errors\":[],\"assemblyBytes\":null}");
            Console.Out.Flush();
        }
        return 0;
    }
}

