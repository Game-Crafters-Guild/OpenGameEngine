// Preloaded into the MCP server by mcp-assistant-check.mjs (node --import): every
// function that writes a file or starts a process throws, after printing one
// "[host-stub] <module>.<function> [<first argument, when it is a path>]" line on stderr
// for the harness to count and to read where it would have written. A tool that
// reaches one of them acted on this machine.

import childProcess from 'node:child_process';
import fs from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';

const kStubbed = {
    child_process: [childProcess, ['spawn', 'spawnSync', 'exec', 'execSync', 'execFile', 'execFileSync', 'fork']],
    fs: [fs, ['writeFileSync', 'writeFile', 'appendFileSync', 'appendFile', 'mkdirSync', 'mkdir', 'rmSync', 'rm',
              'unlinkSync', 'unlink', 'renameSync', 'rename', 'copyFileSync', 'copyFile', 'createWriteStream',
              'rmdirSync', 'rmdir']],
};

for (const [moduleName, [target, names]] of Object.entries(kStubbed)) {
    for (const name of names) {
        target[name] = (first) => {
            const where = typeof first === 'string' ? ` ${first}` : '';
            process.stderr.write(`[host-stub] ${moduleName}.${name}${where}\n`);
            throw new Error(`host-stub: ${moduleName}.${name} is not allowed in this check`);
        };
    }
}
for (const name of ['writeFile', 'appendFile', 'mkdir', 'rm', 'unlink', 'rename', 'copyFile', 'rmdir']) {
    fs.promises[name] = async () => {
        process.stderr.write(`[host-stub] fs.promises.${name}\n`);
        throw new Error(`host-stub: fs.promises.${name} is not allowed in this check`);
    };
}
syncBuiltinESMExports();
