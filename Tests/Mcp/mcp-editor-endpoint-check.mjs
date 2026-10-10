// Every answer that came from an editor must say WHICH editor answered.
//
// Several editors run on one machine: a lane's own, another session's, the
// developer's on the default port. A tool call that lands on the wrong one
// succeeds and returns plausible data — nothing in the response distinguishes
// it from the right one. The fix under test is that both front-ends report the
// endpoint that served the call, taken from the live socket rather than from
// configuration.
//
// "Answer" includes a refusal: an editor that says no is still the editor that
// said it, and which one refused is exactly what a failing call needs to tell
// you. Only genuine silence — nothing listening — may name nobody.
//
// The editor is faked here: a TCP server speaking the debug protocol's
// line-delimited JSON. No editor binary, no GPU, no build of the C++ side.
//
// Exit 0 = every arm held; 1 = a failure (the reported endpoint is the wrong
// one, missing, or invented); 2 = the harness itself could not run.
//
//   node Tests/Mcp/mcp-editor-endpoint-check.mjs

import { spawn } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const geEntry = path.join(repoRoot, 'mcp', 'ge.mjs');

// Deliberately not a port anything here listens on: it stands in for the shared
// default an unconfigured server would reach, and every arm must prove the
// report follows the socket rather than this.
const kDecoyEnvPort = '9';

const failures = [];

function check(label, condition, detail) {
  if (condition) return;
  failures.push(`${label}\n    ${detail}`);
}

/**
 * A TCP server on an OS-chosen port that answers every request with `payload`.
 * `refuse` makes it answer `ok:false` instead — a protocol-level refusal, which
 * is still an editor answering and must still be attributed. The refusal
 * carries details, the diagnostic fields a handler refuses with.
 */
function startFakeEditor(payload, { refuse = false } = {}) {
  return new Promise((resolve, reject) => {
    const server = net.createServer((socket) => {
      let buffer = '';
      socket.on('data', (chunk) => {
        buffer += chunk.toString();
        const lines = buffer.split('\n');
        buffer = lines.pop() ?? '';
        for (const line of lines) {
          if (!line.trim()) continue;
          let id = '0';
          try { id = JSON.parse(line).id ?? '0'; } catch { /* answer anyway */ }
          socket.write(JSON.stringify(refuse
            ? { id, ok: false, error: 'refused by fake editor', details: { candidates: ['HZB', 'HZBMip0'] } }
            : { id, ok: true, result: payload }) + '\n');
        }
      });
      socket.on('error', () => { /* client hangs up between arms */ });
    });
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port }));
  });
}

// One budget for the whole run, not per spawn. The first spawn may install and
// compile mcp/; the rest are fast. A per-spawn timeout multiplied by the arm
// count can exceed the ctest TIMEOUT, and then ctest kills the harness instead
// of the harness reporting — a test that dies without saying why. Kept below
// that TIMEOUT so this file is always the thing that speaks first.
const kTotalBudgetMs = 540_000;
const deadline = Date.now() + kTotalBudgetMs;

/** Spawns the CLI and returns its stdout envelope plus raw stderr. */
function spawnCli(args, { stdin = null } = {}) {
  return new Promise((resolve, reject) => {
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      reject(new Error(`ran out of the ${kTotalBudgetMs}ms budget before: ge ${args.join(' ')}`));
      return;
    }
    const child = spawn(process.execPath, [geEntry, ...args], {
      cwd: repoRoot,
      // The decoy is what an unconfigured front-end would use. --port must win,
      // and the reported endpoint must name where the bytes actually went.
      env: { ...process.env, GE_EDITOR_DEBUG_PORT: kDecoyEnvPort },
      stdio: [stdin === null ? 'ignore' : 'pipe', 'pipe', 'pipe'],
    });
    if (stdin !== null) child.stdin.end(stdin);

    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => { stdout += d.toString(); });
    child.stderr.on('data', (d) => { stderr += d.toString(); });
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`timed out (${remaining}ms of budget left): ge ${args.join(' ')}`));
    }, remaining);
    child.on('error', (e) => { clearTimeout(timer); reject(e); });
    child.on('close', () => { clearTimeout(timer); resolve({ stdout, stderr }); });
  });
}

/** Runs the CLI and returns its parsed --json envelope. */
async function runCli(args) {
  const { stdout, stderr } = await spawnCli([...args, '--json']);
  const line = stdout.trim().split('\n').filter(Boolean).pop();
  if (!line) throw new Error(`no JSON on stdout from: ge ${args.join(' ')}\nstderr:\n${stderr}`);
  try {
    return JSON.parse(line);
  } catch {
    throw new Error(`unparseable stdout from: ge ${args.join(' ')}\n${line}\nstderr:\n${stderr}`);
  }
}

/** A closed port: connecting to it must fail rather than reach anything. */
function findClosedPort() {
  return new Promise((resolve, reject) => {
    const probe = net.createServer();
    probe.on('error', reject);
    probe.listen(0, '127.0.0.1', () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
}

const first = await startFakeEditor({ marker: 'first-editor' });
const second = await startFakeEditor({ marker: 'second-editor' });
const refuser = await startFakeEditor(null, { refuse: true });

function closeFakeEditors() {
  for (const e of [first, second, refuser]) e.server.close();
}

try {
  // Arm 1 — a successful call names the editor that served it, not the port the
  // environment would have used.
  const a = await runCli(['get_camera', '--port', String(first.port)]);
  check('arm 1: call succeeded', a.ok === true, `envelope: ${JSON.stringify(a)}`);
  check('arm 1: reached the fake editor', a.data?.marker === 'first-editor', `data: ${JSON.stringify(a.data)}`);
  check('arm 1: endpoint names the answering editor',
        a.editor === `127.0.0.1:${first.port}`,
        `expected 127.0.0.1:${first.port}, got ${JSON.stringify(a.editor)}`);
  check('arm 1: endpoint is not the environment default',
        !String(a.editor ?? '').endsWith(`:${kDecoyEnvPort}`),
        `endpoint followed GE_EDITOR_DEBUG_PORT instead of the socket: ${a.editor}`);

  // Arm 2 — a second editor on a second port. A constant, a hardcoded default,
  // or a value baked at build time passes arm 1 and fails here.
  const b = await runCli(['get_camera', '--port', String(second.port)]);
  check('arm 2: reached the second fake editor', b.data?.marker === 'second-editor', `data: ${JSON.stringify(b.data)}`);
  check('arm 2: endpoint tracks the target',
        b.editor === `127.0.0.1:${second.port}`,
        `expected 127.0.0.1:${second.port}, got ${JSON.stringify(b.editor)}`);
  check('arm 2: the two arms reported different editors',
        a.editor !== b.editor,
        `both arms reported ${a.editor} — the field is not tracking the connection`);

  // Arm 3 — nothing answered, so no editor may be claimed. An endpoint invented
  // from configuration would be worse than none: it would name an editor that
  // never saw the request.
  const closed = await findClosedPort();
  const c = await runCli(['get_camera', '--port', String(closed)]);
  check('arm 3: unreachable is reported as failure', c.ok === false, `envelope: ${JSON.stringify(c)}`);
  check('arm 3: classified as unreachable', c.code === 'EDITOR_UNREACHABLE', `code: ${c.code}`);
  check('arm 3: no editor claimed when none answered',
        c.editor === undefined || c.editor === null,
        `claimed ${JSON.stringify(c.editor)} though nothing was listening on ${closed}`);

  // Arm 3b — a refusal is an answer. An ok:false response rejects the request
  // promise, and attribution that only fires on the resolve path would drop
  // exactly the case where "which editor said no" is the question being asked.
  // The contrast with arm 3 is the point: refused names an editor, unreachable
  // must not.
  const r = await runCli(['get_camera', '--port', String(refuser.port)]);
  check('arm 3b: refusal is reported as failure', r.ok === false, `envelope: ${JSON.stringify(r)}`);
  check('arm 3b: the refusing editor is named',
        r.editor === `127.0.0.1:${refuser.port}`,
        `expected 127.0.0.1:${refuser.port}, got ${JSON.stringify(r.editor)} — an ok:false answer went unattributed`);
  check('arm 3b: the refusal reason reaches the caller',
        String(r.error ?? '').includes('refused by fake editor'), `error: ${JSON.stringify(r.error)}`);
  check('arm 3b: the refusal details reach the caller',
        String(r.error ?? '').includes('HZBMip0'), `error: ${JSON.stringify(r.error)}`);

  // Arm 4 — a tool that reaches the editor through the control-plane helpers
  // rather than through editorCall must still be attributed. Readiness polling,
  // port freeing and shutdown drive the transport directly for their own
  // timeouts, and they are the tools whose whole job is to say which editor you
  // just attached to — the class most costly to leave unattributed.
  const d = await runCli(['wait_for_editor', '--port', String(first.port)]);
  check('arm 4: control-plane tool succeeded', d.ok === true, `envelope: ${JSON.stringify(d)}`);
  check('arm 4: control-plane tool names its editor',
        d.editor === `127.0.0.1:${first.port}`,
        `expected 127.0.0.1:${first.port}, got ${JSON.stringify(d.editor)} — a path that bypasses editorCall is unattributed`);

  // Arm 5 — the endpoint comes from the socket, not from configuration. The
  // arms above cannot tell those apart (the CLI sets the config before
  // connecting, so they agree), and an established connection keeps the port it
  // was opened with: a later override leaves the config naming an editor this
  // client is not talking to. Driven in-process because no front-end can
  // produce that divergence. mcp/dist exists by now — the arms above built it.
  const { IpcClient } = await import(pathToFileURL(path.join(repoRoot, 'mcp', 'dist', 'ipc-client.js')).href);
  const { overrideIpcConfig } = await import(pathToFileURL(path.join(repoRoot, 'mcp', 'dist', 'config.js')).href);

  overrideIpcConfig({ port: first.port, host: '127.0.0.1' });
  const client = new IpcClient();
  await client.request('get_camera', {});
  check('arm 5: endpoint reported while connected',
        client.remoteEndpoint === `127.0.0.1:${first.port}`,
        `expected 127.0.0.1:${first.port}, got ${JSON.stringify(client.remoteEndpoint)}`);

  overrideIpcConfig({ port: Number(kDecoyEnvPort) });
  check('arm 5: endpoint follows the open socket, not the reconfigured port',
        client.remoteEndpoint === `127.0.0.1:${first.port}`,
        `config moved to ${kDecoyEnvPort} and the endpoint followed it: ${JSON.stringify(client.remoteEndpoint)}`);

  client.disconnect();
  check('arm 5: no endpoint once the socket is gone',
        client.remoteEndpoint === null,
        `still naming ${JSON.stringify(client.remoteEndpoint)} after disconnect`);

  // Arm 6 — `ge batch` owns its socket instead of using IpcClient, so the
  // attribution the other arms exercise cannot reach it. Its stdout is a wire
  // protocol consumers parse by id, so the endpoint belongs on stderr — but it
  // has to be somewhere, or the one path that streams many calls at an editor
  // is the one path that never says which editor.
  const batch = await spawnCli(['batch', '--port', String(second.port)],
                               { stdin: JSON.stringify({ method: 'get_camera', params: {} }) + '\n' });
  check('arm 6: batch relayed the editor response on stdout',
        batch.stdout.includes('second-editor'),
        `stdout: ${batch.stdout}`);
  check('arm 6: batch names its editor on stderr',
        batch.stderr.includes(`[editor 127.0.0.1:${second.port}]`),
        `expected [editor 127.0.0.1:${second.port}] on stderr, got: ${JSON.stringify(batch.stderr)}`);
  check('arm 6: batch kept its stdout free of the endpoint',
        !batch.stdout.includes('[editor '),
        `the wire protocol gained a non-protocol line: ${batch.stdout}`);
} catch (err) {
  console.error(`harness error: ${err.message}`);
  closeFakeEditors();
  process.exit(2);
}

closeFakeEditors();

if (failures.length > 0) {
  console.error(`FAIL — ${failures.length} check(s):`);
  for (const f of failures) console.error(`  ${f}`);
  process.exit(1);
}

console.log('OK — every editor-backed response named the editor that answered.');
