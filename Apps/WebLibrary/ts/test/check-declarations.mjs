// The declarations gate: the public .d.ts files, the type tests under test/types and the
// documentation's examples compile under `tsc --noEmit`, so a declared surface that
// contradicts itself, a type test whose misuse stops being an error, or an example the
// declarations no longer accept, fails.
//
// Exit 0 = the declarations compile; 1 = tsc reported errors; 2 = the gate could not run.
//
//   node Apps/WebLibrary/ts/test/check-declarations.mjs

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const packageDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function run(command, args) {
    // shell on Windows: npm is npm.cmd, and spawning a .cmd needs a shell.
    return spawnSync(command, args, { cwd: packageDir, stdio: 'inherit', shell: process.platform === 'win32' });
}

if (!fs.existsSync(path.join(packageDir, 'node_modules', 'typescript'))) {
    const install = run('npm', ['ci', '--no-audit', '--no-fund']);
    if (install.status !== 0) {
        console.error(`[declarations] npm ci failed in ${packageDir}; run it by hand to see why.`);
        process.exit(2);
    }
}

const tsc = path.join(packageDir, 'node_modules', 'typescript', 'bin', 'tsc');

function compile(project) {
    const result = spawnSync(process.execPath, [tsc, '-p', project, '--noEmit'], { cwd: packageDir, stdio: 'inherit' });
    if (result.error) {
        console.error(`[declarations] could not start tsc: ${result.error.message}`);
        process.exit(2);
    }
    return result.status === 0;
}

const kFence = '```';
const kExampleBlock = new RegExp(`${kFence}js\\r?\\n([\\s\\S]*?)${kFence}`, 'g');

function toPosix(file) {
    return file.split(path.sep).join('/');
}

// The README's and llms.txt's examples, compiled as written: each js block becomes a module
// checked against the declarations, so the documentation cannot drift from them.
function writeDocumentationExamples() {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'opengine-doc-examples-'));
    const files = [];
    for (const document of ['README.md', 'llms.txt']) {
        const text = fs.readFileSync(path.join(packageDir, '..', document), 'utf8');
        let index = 0;
        for (const match of text.matchAll(kExampleBlock)) {
            const file = path.join(directory, `${document.replace('.', '-')}-${++index}.js`);
            fs.writeFileSync(file, match[1]);
            files.push(file);
        }
    }
    if (files.length === 0) {
        console.error('[declarations] found no js example in README.md or llms.txt');
        process.exit(1);
    }
    const project = path.join(directory, 'tsconfig.json');
    fs.writeFileSync(project, JSON.stringify({
        extends: toPosix(path.join(packageDir, 'tsconfig.json')),
        include: files.map(toPosix),
    }));
    console.log(`[declarations] ${files.length} documentation examples`);
    return { directory, project };
}

const declarationsPass = compile('tsconfig.json');
const examples = writeDocumentationExamples();
const examplesPass = compile(examples.project);
fs.rmSync(examples.directory, { recursive: true, force: true });
const pass = declarationsPass && examplesPass;
console.log(pass ? '[declarations] PASS' : '[declarations] FAIL');
process.exit(pass ? 0 : 1);
