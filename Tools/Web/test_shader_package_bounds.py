"""Host package contract: the cook reads and writes packages only through the
ShaderReflect tool, and a malformed package fails there, before any cooking.

The container's own bounds checks are covered exhaustively by the engine's
ShaderPackageContainer tests; this checks that the cook's helpers surface the
tool's refusal as a CookFailure and round-trip what they append.
"""
import argparse
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import builtin_shader_cook as cook

kArgs = argparse.Namespace()


class ShaderPackageBoundsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.reflector = kArgs.shader_reflect
        self.path = self.dir / 'bounds.shaderpkg'
        shutil.copyfile(kArgs.shaderpkg_dir / 'encode_srgb.shaderpkg', self.path)
        self.wgsl = self.dir / 'fs.wgsl'
        self.wgsl.write_bytes(b'@fragment fn main() {}')
        cook.AppendShaderPkgChunks(self.reflector, self.path, [('fs-wgsl', self.wgsl)])
        self.valid = self.path.read_bytes()

    def test_round_trip(self):
        names = [chunk['name'] for chunk in cook.ListShaderPkgChunks(self.reflector, self.path)]
        self.assertEqual(names[0], 'meta-bin')
        self.assertEqual(names[-1], 'fs-wgsl')
        self.assertEqual(cook.ReadShaderPkgChunk(self.reflector, self.path, 'fs-wgsl'),
                         self.wgsl.read_bytes())
        self.assertIn('sets', cook.ReadShaderPkgMeta(self.reflector, self.path))

    def test_a_repeated_chunk_is_refused(self):
        with self.assertRaises(cook.CookFailure):
            cook.AppendShaderPkgChunks(self.reflector, self.path, [('fs-wgsl', self.wgsl)])
        self.assertEqual(self.path.read_bytes(), self.valid)

    def test_truncated_packages_are_refused(self):
        # Inside the header, inside the chunk table, and one byte short of the end.
        for length in (0, 15, 16 + 20, len(self.valid) - 1):
            with self.subTest(length=length):
                self.path.write_bytes(self.valid[:length])
                with self.assertRaises(cook.CookFailure):
                    cook.ListShaderPkgChunks(self.reflector, self.path)

    def test_impossible_chunk_count_is_refused(self):
        data = bytearray(self.valid)
        struct.pack_into('<I', data, 12, 0xffffffff)
        self.path.write_bytes(data)
        with self.assertRaises(cook.CookFailure):
            cook.ListShaderPkgChunks(self.reflector, self.path)

    def test_another_version_is_refused(self):
        data = bytearray(self.valid)
        struct.pack_into('<I', data, 8, 1)
        self.path.write_bytes(data)
        with self.assertRaises(cook.CookFailure):
            cook.ReadShaderPkgMeta(self.reflector, self.path)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shader-reflect', type=Path, required=True)
    parser.add_argument('--shaderpkg-dir', type=Path, required=True)
    kArgs, remaining = parser.parse_known_args(namespace=kArgs)
    unittest.main(argv=[sys.argv[0], *remaining])
