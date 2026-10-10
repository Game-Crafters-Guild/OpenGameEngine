// The facade's node tests: builds the facade and the stubbed ABI with tsc into dist/, then
// runs every test/*.test.mjs under node's test runner in one process. Pass
// --junit <file> to also write the results as JUnit XML.
//
// Exit 0 = every test passed; 1 = a test or the build failed; 2 = the gate could not run.
//
//   node Apps/WebLibrary/ts/test/run-tests.mjs [--junit C:/path/results.xml]

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const packageDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

if (!fs.existsSync(path.join(packageDir, 'node_modules', 'typescript'))) {
    // shell on Windows: npm is npm.cmd, and spawning a .cmd needs a shell.
    const install = spawnSync('npm', ['ci', '--no-audit', '--no-fund'], { cwd: packageDir, stdio: 'inherit', shell: process.platform === 'win32' });
    if (install.status !== 0) {
        console.error(`[facade] npm ci failed in ${packageDir}; run it by hand to see why.`);
        process.exit(2);
    }
}

fs.rmSync(path.join(packageDir, 'dist'), { recursive: true, force: true });
const tsc = path.join(packageDir, 'node_modules', 'typescript', 'bin', 'tsc');
const build = spawnSync(process.execPath, [tsc, '-p', 'tsconfig.build.json'], { cwd: packageDir, stdio: 'inherit' });
if (build.error) {
    console.error(`[facade] could not start tsc: ${build.error.message}`);
    process.exit(2);
}
if (build.status !== 0) {
    console.error('[facade] FAIL: the facade does not build');
    process.exit(1);
}

const testDir = path.join(packageDir, 'test');
const tests = fs.readdirSync(testDir).filter((f) => f.endsWith('.test.mjs')).map((f) => path.join(testDir, f));
const reporters = ['--test-reporter=spec', '--test-reporter-destination=stdout'];
const junitAt = process.argv.indexOf('--junit');
if (junitAt >= 0) reporters.push('--test-reporter=junit', `--test-reporter-destination=${process.argv[junitAt + 1]}`);
const run = spawnSync(process.execPath, ['--test', '--test-isolation=none', ...reporters, ...tests], { cwd: packageDir, stdio: 'inherit' });
if (run.error) {
    console.error(`[facade] could not start the test runner: ${run.error.message}`);
    process.exit(2);
}
console.log(run.status === 0 ? '[facade] PASS' : '[facade] FAIL');
process.exit(run.status === 0 ? 0 : 1);
