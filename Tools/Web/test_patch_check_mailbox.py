#!/usr/bin/env python3
"""Exercise the patched mailbox callbacks across asyncify and pthread lifetimes."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PATCHER = Path(__file__).with_name("patch_check_mailbox.py")
# The relevant emitted-runtime shape, with controllable scheduling and thread
# identity. The assertions exercise callbacks after applying the real patcher.
RUNTIME = '''
const assert = require("node:assert/strict");
const pending = [];
const calls = [];
let currentThread = 101;
const Asyncify = {state: 0, currData: null, exportCallStack: []};
const setTimeout = (callback) => pending.push(callback);
const _pthread_self = () => currentThread;
const callUserCallback = (callback) => callback();
const __emscripten_thread_mailbox_await = (thread) => calls.push(["await", thread]);
const __emscripten_check_mailbox = () => calls.push(["pump", currentThread]);
var checkMailbox=()=>{var pthread_ptr=_pthread_self();if(!pthread_ptr)return;
callUserCallback(()=>{__emscripten_thread_mailbox_await(pthread_ptr);__emscripten_check_mailbox()})};
const runNext = () => { assert.ok(pending.length); pending.shift()(); };
'''


@unittest.skipUnless(shutil.which("node"), "Node is required to exercise the emitted runtime")
class MailboxPatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "WebEditor.js"
        self.path.write_text(RUNTIME)
        self.patch()

    def patch(self):
        import sys
        result = subprocess.run([sys.executable, str(PATCHER), str(self.path)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def run_case(self, case):
        result = subprocess.run(["node", "-e", self.path.read_text() + case],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_idle_thread_pumps_from_a_fresh_task(self):
        self.run_case('''
checkMailbox(); assert.deepEqual(calls, []);
runNext(); assert.deepEqual(calls, []);
runNext(); assert.deepEqual(calls, [["await", 101], ["pump", 101]]);
''')

    def test_delayed_callback_does_not_use_an_exited_thread(self):
        self.run_case('''
checkMailbox(); runNext(); currentThread = 0;
runNext(); assert.deepEqual(calls, []); assert.equal(pending.length, 0);
''')

    def test_delayed_callback_does_not_cross_worker_reuse(self):
        self.run_case('''
checkMailbox(); runNext(); currentThread = 202;
runNext(); assert.deepEqual(calls, []);
checkMailbox(); runNext(); runNext();
assert.deepEqual(calls, [["await", 202], ["pump", 202]]);
''')

    def test_suspended_continuation_defers_the_outer_pump(self):
        self.run_case('''
Asyncify.currData = 123; checkMailbox(); runNext();
assert.deepEqual(calls, []); assert.equal(pending.length, 1);
Asyncify.currData = null; runNext(); runNext();
assert.deepEqual(calls, [["await", 101], ["pump", 101]]);
''')

    def test_nested_wasm_entry_defers_the_inner_pump(self):
        self.run_case('''
checkMailbox(); runNext(); Asyncify.exportCallStack.push("frame");
runNext(); assert.deepEqual(calls, []);
Asyncify.exportCallStack.pop(); runNext();
assert.deepEqual(calls, [["await", 101], ["pump", 101]]);
''')

    def test_thread_reuse_during_suspension_drops_the_old_callback(self):
        self.run_case('''
checkMailbox(); runNext(); Asyncify.state = 1;
runNext(); assert.deepEqual(calls, []);
currentThread = 202; Asyncify.state = 0;
runNext(); assert.deepEqual(calls, []); assert.equal(pending.length, 0);
''')

    def test_reapplying_patch_is_idempotent(self):
        original = self.path.read_text()
        self.patch()
        self.assertEqual(self.path.read_text(), original)


if __name__ == "__main__":
    unittest.main()
