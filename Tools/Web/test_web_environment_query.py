"""Execute the browser's actual URL packer against normal and malformed inputs."""
import argparse
import json
from pathlib import Path
import subprocess
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--repo-root', type=Path, default=Path(__file__).resolve().parents[2])
args, remaining = parser.parse_known_args()
source = (args.repo_root/'Engine/Modules/Platform/Source/WebEnvironment_Web.cpp').read_text(encoding='utf-8')
body = source.split('EM_ASM_PTR({', 1)[1].split('}));', 1)[0]


class WebEnvironmentQueryTests(unittest.TestCase):
    def pack(self, pairs):
        script = '''
const body = JSON.parse(process.argv[1]);
const pairs = JSON.parse(process.argv[2]);
const run = new Function('globalThis', 'stringToNewUTF8', body);
const value = run({location: {search: '?' + new URLSearchParams(pairs)}}, s => s);
process.stdout.write(JSON.stringify(value));
'''
        r = subprocess.run(['node', '-e', script, json.dumps(body), json.dumps(pairs)],
                           capture_output=True, text=True, encoding='utf-8', timeout=15, check=True)
        return json.loads(r.stdout)

    def test_only_engine_environment_is_imported(self):
        self.assertEqual(self.pack([['PATH', 'ignored'], ['GE_MODE', 'full']]), 'GE_MODE\0full\0')

    def test_nul_in_name_cannot_create_an_extra_pair(self):
        self.assertEqual(self.pack([['GE_BAD\0value\0PATH', 'injected'], ['GE_OK', 'yes']]),
                         'GE_OK\0yes\0')

    def test_nul_in_value_cannot_break_pair_boundaries(self):
        self.assertEqual(self.pack([['GE_BAD', 'first\0PATH\0injected'], ['GE_OK', 'yes']]),
                         'GE_OK\0yes\0')

    def test_odd_nul_value_cannot_consume_the_final_terminator(self):
        self.assertEqual(self.pack([['GE_BAD', 'odd\0tail']]), 0)

    def test_empty_and_unicode_values_remain_valid(self):
        self.assertEqual(self.pack([['GE_EMPTY', ''], ['GE_TEXT', '\u00e9 snow\u2603']]),
                         'GE_EMPTY\0\0GE_TEXT\0\u00e9 snow\u2603\0')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *remaining])
