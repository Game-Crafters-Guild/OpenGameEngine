param(
    [Parameter(Mandatory = $true)]
    [string]$Objlist,

    [Parameter(Mandatory = $true)]
    [string]$Def
)

$ErrorActionPreference = 'Stop'

$savedLib = $env:LIB
$env:LIB = ''
try {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class CoffExportFilter
{
    private const byte ExternalStorageClass = 2;
    private static readonly Encoding Ascii = Encoding.ASCII;

    public static string Run(string objlistPath, string defPath)
    {
        HashSet<string> wanted = ReadWantedExports(defPath);
        HashSet<string> defined = new HashSet<string>(StringComparer.Ordinal);
        int objectsScanned = 0;

        foreach (string obj in File.ReadLines(objlistPath))
        {
            if (string.IsNullOrWhiteSpace(obj) || !File.Exists(obj))
                continue;

            AddDefinedSymbolsFromObject(obj, wanted, defined, ref objectsScanned);
            if (defined.Count == wanted.Count)
                break;
        }

        List<string> kept = new List<string>();
        int inputCount = 0;
        int removedUndefined = 0;

        foreach (string line in File.ReadLines(defPath))
        {
            ++inputCount;
            string trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed == "EXPORTS")
            {
                kept.Add(line);
                continue;
            }

            string symbol = FirstToken(trimmed);
            if (defined.Contains(symbol))
                kept.Add(line);
            else
                ++removedUndefined;
        }

        File.WriteAllLines(defPath, kept.ToArray(), new UTF8Encoding(false));
        return string.Format(
            "objects={0}, wanted={1}, defined={2}, input={3}, kept={4}, removed-undefined={5}",
            objectsScanned,
            wanted.Count,
            defined.Count,
            inputCount,
            kept.Count,
            removedUndefined);
    }

    private static HashSet<string> ReadWantedExports(string defPath)
    {
        HashSet<string> wanted = new HashSet<string>(StringComparer.Ordinal);
        foreach (string line in File.ReadLines(defPath))
        {
            string trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed == "EXPORTS")
                continue;

            wanted.Add(FirstToken(trimmed));
        }
        return wanted;
    }

    private static string FirstToken(string text)
    {
        int end = 0;
        while (end < text.Length && !char.IsWhiteSpace(text[end]))
            ++end;
        return end == text.Length ? text : text.Substring(0, end);
    }

    private static void AddDefinedSymbolsFromObject(
        string path,
        HashSet<string> wanted,
        HashSet<string> defined,
        ref int objectsScanned)
    {
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        using (BinaryReader br = new BinaryReader(fs))
        {
            if (fs.Length < 20)
                return;

            ushort first = br.ReadUInt16();
            ushort second = br.ReadUInt16();
            bool isBigObj = (first == 0 && second == 0xffff);

            int recordSize;
            uint symbolTableOffset;
            uint symbolCount;
            if (isBigObj)
            {
                if (fs.Length < 56)
                    return;

                recordSize = 20;
                fs.Position = 48;
                symbolTableOffset = br.ReadUInt32();
                symbolCount = br.ReadUInt32();
            }
            else
            {
                recordSize = 18;
                fs.Position = 8;
                symbolTableOffset = br.ReadUInt32();
                symbolCount = br.ReadUInt32();
            }

            if (symbolTableOffset == 0 || symbolCount == 0)
                return;

            long stringTableOffset = (long)symbolTableOffset + ((long)recordSize * symbolCount);
            if (stringTableOffset + 4 > fs.Length)
                return;

            fs.Position = stringTableOffset;
            uint stringTableSize = br.ReadUInt32();
            byte[] stringBytes = new byte[0];
            if (stringTableSize > 4 &&
                stringTableSize <= int.MaxValue &&
                stringTableOffset + stringTableSize <= fs.Length)
            {
                stringBytes = br.ReadBytes((int)stringTableSize - 4);
            }

            fs.Position = symbolTableOffset;
            for (uint i = 0; i < symbolCount;)
            {
                if (fs.Position + recordSize > fs.Length)
                    break;

                byte[] nameBytes = br.ReadBytes(8);
                if (nameBytes.Length != 8)
                    break;

                br.ReadUInt32();
                int sectionNumber = isBigObj ? br.ReadInt32() : br.ReadInt16();
                br.ReadUInt16();
                byte storageClass = br.ReadByte();
                byte auxCount = br.ReadByte();

                if (storageClass == ExternalStorageClass && sectionNumber > 0)
                {
                    string name = GetSymbolName(nameBytes, stringBytes);
                    if (name.Length > 0 && wanted.Contains(name))
                        defined.Add(name);
                }

                if (auxCount > 0)
                {
                    long next = fs.Position + ((long)recordSize * auxCount);
                    fs.Position = Math.Min(fs.Length, next);
                }
                i += (uint)(1 + auxCount);
            }

            ++objectsScanned;
        }
    }

    private static string GetSymbolName(byte[] nameBytes, byte[] stringBytes)
    {
        uint zeroes = BitConverter.ToUInt32(nameBytes, 0);
        if (zeroes == 0)
        {
            uint offset = BitConverter.ToUInt32(nameBytes, 4);
            if (offset < 4)
                return string.Empty;

            long stringStart = (long)offset - 4;
            if (stringStart < 0 || stringStart >= stringBytes.Length)
                return string.Empty;

            return ReadNullTerminatedAscii(stringBytes, (int)stringStart, stringBytes.Length - (int)stringStart);
        }

        return ReadNullTerminatedAscii(nameBytes, 0, 8);
    }

    private static string ReadNullTerminatedAscii(byte[] bytes, int start, int maxLength)
    {
        if (start < 0 || start >= bytes.Length || maxLength <= 0)
            return string.Empty;

        int limit = Math.Min(bytes.Length, start + maxLength);
        int end = start;
        while (end < limit && bytes[end] != 0)
            ++end;

        return end <= start ? string.Empty : Ascii.GetString(bytes, start, end - start);
    }
}
'@
} finally {
    $env:LIB = $savedLib
}

[CoffExportFilter]::Run($Objlist, $Def)
