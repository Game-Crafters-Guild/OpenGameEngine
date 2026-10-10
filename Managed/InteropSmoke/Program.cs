using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using GameEngine;

class Program
{
    public static int Main(string[] args)
    {
        string msg = "[InteropSmoke] Hello UTF-8 π";
        var utf8 = Encoding.UTF8;
        int byteLen = utf8.GetByteCount(msg);
        byte[] buffer = new byte[byteLen];
        int written = utf8.GetBytes(msg, 0, msg.Length, buffer, 0);

        // Use managed facade: log via Engine and read ECS world count
        var world = Engine.Current.Worlds.Primary;
        Console.WriteLine($"World entity count: {world.EntityCount}");
        // Basic smoke success
        return 0;
    }
}

