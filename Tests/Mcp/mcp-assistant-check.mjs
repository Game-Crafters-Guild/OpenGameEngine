// The MCP server as the editor's AI Assistant attaches it: GE_MCP_TOOLS picks the
// tools and resources it serves, GE_ASSISTANT_TOKEN binds every connection to the
// assistant's session before anything else is sent, and a tool that acts on this
// machine asks the editor's gate (assistant_authorize) before it runs.
//
// Arms:
//   * tools/list and resources/list are exactly the GE_MCP_TOOLS set;
//   * an unknown name in GE_MCP_TOOLS stops the server at start, naming it;
//   * the first request on the connection is assistant_bind with the token;
//   * a refused bind makes every later call return the refusal and reach no
//     other method;
//   * the inventory: every tool in the registry, run with file writes and process
//     starts stubbed to throw (assistant-host-stub.mjs), either makes at most one
//     editor call and no write or start, or is marked actsOnHost and makes
//     assistant_authorize first and nothing else when it is refused. A tool whose
//     sample arguments reach a write or a start without the mark fails here; one
//     that fails earlier on its sample (a graph path that does not exist) is caught
//     by the class-table arm;
//   * the editor's class table (Packages/ai-assistant/Editor/AssistantTools.cpp) and
//     this registry agree: the same names, each row's method is the one the tool
//     sends, a Gated row with no method is actsOnHost, and no actsOnHost tool is
//     in a class that runs without asking (Read, View, UndoableEdit, Input);
//   * GE_PROJECT_ROOT: a host-side tool resolves its path arguments against the
//     project, refuses a path outside it, writes its defaults inside it, and a
//     generated action that exists already is replaced only with overwrite; an
//     empty GE_PROJECT_ROOT (no project open) refuses them;
//
// The editor is a fake TCP server speaking the debug protocol; no editor binary.
// Runs the built server (mcp/dist), which ge.mjs brings up to date first.
//
// Exit 0 = every arm held; 1 = a failure; 2 = the harness could not run.
//
//   node Tests/Mcp/mcp-assistant-check.mjs

import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..');
const serverEntry = path.join(repoRoot, 'mcp', 'dist', 'index.js');
const hostStub = pathToFileURL(path.join(here, 'assistant-host-stub.mjs')).href;
const kGoodToken = 'assistant-check-token';
const kCallBudgetMs = 20_000;
const kAmdTooling = {
    vendor: 'AMD', vendorId: 4098, hardware: 'AMD Radeon RX 7900 XTX', debugUtilsEnabled: true,
    validationLayerEnabled: false, toolingInfoAvailable: true, attachedTools: [],
};
// RDP_PATH names a directory holding both Radeon developer CLIs, so rgp_capture
// resolves its suite instead of refusing before it would start one.
const exeSuffix = process.platform === 'win32' ? '.exe' : '';
const radeonSuite = fs.mkdtempSync(path.join(os.tmpdir(), 'assistant-check-rdp-'));
for (const cli of ['RadeonDeveloperServiceCLI', 'RadeonDeveloperPanelCLI'])
    fs.writeFileSync(path.join(radeonSuite, cli + exeSuffix), '');

// ge.mjs rebuilds mcp/dist when mcp/src is newer, so the server under test is the source's.
const prebuild = spawnSync(process.execPath, [path.join(repoRoot, 'mcp', 'ge.mjs'), '--help'],
                           { cwd: repoRoot, stdio: ['ignore', 'ignore', 'inherit'] });
if (prebuild.status !== 0 || !fs.existsSync(serverEntry)) {
    console.error(`could not build ${serverEntry} (ge.mjs exit ${prebuild.status})`);
    process.exit(2);
}

const failures = [];
function check(label, condition, detail) {
    if (!condition) failures.push(`${label}\n    ${detail}`);
}

// A fake editor that records every request as {connection, method, params}. It
// binds kGoodToken, refuses any other token, refuses assistant_authorize unless
// `admitAuthorize` is set on the returned object, reports
// an AMD device to get_gpu_tooling (so rgp_capture's vendor gate passes and the
// tool goes as far as it would on that hardware), and answers everything else
// with an empty result.
function startFakeEditor() {
    const requests = [];
    const fake = { requests, admitAuthorize: false };
    let connections = 0;
    return new Promise((resolve, reject) => {
        const server = net.createServer((socket) => {
            const connection = ++connections;
            let buffer = '';
            socket.on('data', (chunk) => {
                buffer += chunk.toString();
                const lines = buffer.split('\n');
                buffer = lines.pop() ?? '';
                for (const line of lines) {
                    if (!line.trim()) continue;
                    const request = JSON.parse(line);
                    requests.push({ connection, method: request.method, params: request.params ?? {} });
                    let answer = { id: request.id, ok: true, result: {} };
                    if (request.method === 'assistant_bind' && request.params?.token !== kGoodToken)
                        answer = { id: request.id, ok: false, error: 'unknown assistant token' };
                    if (request.method === 'assistant_authorize' && !fake.admitAuthorize)
                        answer = { id: request.id, ok: false, error: 'refused by the fake gate' };
                    if (request.method === 'get_gpu_tooling')
                        answer = { id: request.id, ok: true, result: kAmdTooling };
                    socket.write(JSON.stringify(answer) + '\n');
                }
            });
            socket.on('error', () => {});
        });
        server.on('error', reject);
        server.listen(0, '127.0.0.1', () => resolve(Object.assign(fake, { server, port: server.address().port })));
    });
}

// One MCP server process over stdio, spoken to in newline-delimited JSON-RPC.
class McpProcess {
    constructor(env) {
        this.child = spawn(process.execPath, ['--import', hostStub, serverEntry], {
            cwd: repoRoot,
            env: { ...process.env, ...env },
            stdio: ['pipe', 'pipe', 'pipe'],
        });
        this.stderr = '';
        this.pending = new Map();
        this.nextId = 0;
        let buffer = '';
        this.child.stdout.on('data', (chunk) => {
            buffer += chunk.toString();
            const lines = buffer.split('\n');
            buffer = lines.pop() ?? '';
            for (const line of lines) {
                if (!line.trim()) continue;
                const message = JSON.parse(line);
                const waiter = this.pending.get(message.id);
                if (waiter) {
                    this.pending.delete(message.id);
                    waiter(message);
                }
            }
        });
        this.child.stderr.on('data', (chunk) => { this.stderr += chunk.toString(); });
        this.exited = new Promise((resolve) => this.child.on('close', (code) => resolve(code)));
    }

    request(method, params) {
        const id = ++this.nextId;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error(`no answer to ${method} in ${kCallBudgetMs} ms`)), kCallBudgetMs);
            this.pending.set(id, (message) => { clearTimeout(timer); resolve(message); });
            this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
        });
    }

    async initialize() {
        await this.request('initialize', {
            protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'assistant-check', version: '1' },
        });
        this.child.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
    }

    hostActions() {
        return [...this.stderr.matchAll(/\[host-stub\] (\S+)/g)].map(m => m[1]);
    }

    // The paths the stubbed writes were given, in order.
    hostPaths() {
        return [...this.stderr.matchAll(/\[host-stub\] \S+ ([^\r\n]+)/g)].map(m => m[1]);
    }

    stop() {
        this.child.kill();
        return this.exited;
    }
}

// The smallest arguments a JSON schema accepts: each required property filled.
function sampleValue(schema) {
    if (!schema) return 'x';
    if (schema.enum) return schema.enum[0];
    if (schema.anyOf) return sampleValue(schema.anyOf[0]);
    switch (schema.type) {
        case 'string': return 'Sample';
        case 'number': case 'integer': return 1;
        case 'boolean': return false;
        case 'array': return Array.from({ length: schema.minItems ?? 0 }, () => sampleValue(schema.items));
        case 'object': return sampleArgs(schema);
        default: return 'x';
    }
}
function sampleArgs(schema) {
    const args = {};
    for (const name of schema?.required ?? []) args[name] = sampleValue(schema.properties?.[name]);
    return args;
}

const textOf = (result) => (result?.result?.content ?? []).map(c => c.text ?? '').join('\n');

try {
    const editor = await startFakeEditor();
    const assistantEnv = { GE_EDITOR_DEBUG_PORT: String(editor.port), GE_ASSISTANT_TOKEN: kGoodToken,
                           GE_MCP_IPC_TIMEOUT_MS: '5000', RDP_PATH: radeonSuite };

    // The filter: tools and resources are exactly the set.
    {
        const set = ['get_log', 'set_component', 'create_component'];
        const mcp = new McpProcess({ ...assistantEnv, GE_MCP_TOOLS: set.join(',') });
        await mcp.initialize();
        const tools = (await mcp.request('tools/list', {})).result.tools.map(t => t.name).sort();
        check('tools/list is exactly GE_MCP_TOOLS', JSON.stringify(tools) === JSON.stringify([...set].sort()),
              `listed ${tools.join(', ')}`);
        const resources = (await mcp.request('resources/list', {})).result.resources.map(r => r.name).sort();
        check('resources follow their tools', JSON.stringify(resources) === JSON.stringify(['log']),
              `listed ${resources.join(', ')} (only get_log of the three resource tools is in the set)`);
        await mcp.stop();
    }

    // An unknown name stops the server at start.
    {
        const mcp = new McpProcess({ ...assistantEnv, GE_MCP_TOOLS: 'get_log,no_such_tool' });
        // A server that accepts the name keeps running; it must fail here, not hang the run.
        const kStillRunning = 'still running';
        let timer;
        const code = await Promise.race([mcp.exited, new Promise((resolve) => { timer = setTimeout(() => resolve(kStillRunning), 10_000); })]);
        clearTimeout(timer);
        if (code === kStillRunning) {
            check('an unknown GE_MCP_TOOLS name fails the start', false,
                  'the server kept running for 10 s with an unknown name in GE_MCP_TOOLS');
            await mcp.stop();
        } else {
            check('an unknown GE_MCP_TOOLS name fails the start', code !== 0 && mcp.stderr.includes('no_such_tool'),
                  `exit ${code}, stderr: ${mcp.stderr.trim()}`);
        }
    }

    // The first request on a connection is the binding.
    {
        editor.requests.length = 0;
        const mcp = new McpProcess(assistantEnv);
        await mcp.initialize();
        await mcp.request('tools/call', { name: 'get_log', arguments: {} });
        const first = editor.requests[0];
        check('assistant_bind comes first', first?.method === 'assistant_bind' && first.params.token === kGoodToken,
              `requests: ${editor.requests.map(r => r.method).join(', ')}`);
        check('the call follows the binding', editor.requests[1]?.method === 'get_log',
              `requests: ${editor.requests.map(r => r.method).join(', ')}`);
        await mcp.stop();
    }

    // A refused binding refuses every call and sends nothing else.
    {
        editor.requests.length = 0;
        const mcp = new McpProcess({ ...assistantEnv, GE_ASSISTANT_TOKEN: 'stale-token' });
        await mcp.initialize();
        const first = await mcp.request('tools/call', { name: 'get_log', arguments: {} });
        const second = await mcp.request('tools/call', { name: 'get_scene_hierarchy', arguments: {} });
        for (const [label, answer] of [['first', first], ['second', second]]) {
            check(`a refused bind refuses the ${label} call`,
                  answer.result?.isError && textOf(answer).includes("did not accept the assistant's session (unknown assistant token)"),
                  textOf(answer) || JSON.stringify(answer));
        }
        const methods = editor.requests.map(r => r.method);
        check('a refused bind reaches no other method', methods.length === 1 && methods[0] === 'assistant_bind',
              `requests: ${methods.join(', ')}`);
        await mcp.stop();
    }

    // The inventory.
    {
        const mcp = new McpProcess(assistantEnv);
        await mcp.initialize();
        const tools = (await mcp.request('tools/list', {})).result.tools;
        const marked = new Set();
        for (const name of (await import(pathToFileURL(path.join(repoRoot, 'mcp', 'dist', 'tools', 'index.js')).href))
                 .allTools.filter(t => t.actsOnHost).map(t => t.name))
            marked.add(name);
        for (const tool of tools) {
            const before = editor.requests.length;
            const actionsBefore = mcp.hostActions().length;
            await mcp.request('tools/call', { name: tool.name, arguments: sampleArgs(tool.inputSchema) });
            const methods = editor.requests.slice(before).map(r => r.method).filter(m => m !== 'assistant_bind');
            const actions = mcp.hostActions().slice(actionsBefore);
            if (marked.has(tool.name)) {
                check(`${tool.name} asks before it acts`,
                      methods.length === 1 && methods[0] === 'assistant_authorize' && actions.length === 0,
                      `editor calls: [${methods.join(', ')}], host actions: [${actions.join(', ')}]`);
            } else {
                check(`${tool.name} makes at most one editor call and acts on nothing`,
                      methods.length <= 1 && !methods.includes('assistant_authorize') && actions.length === 0,
                      `editor calls: [${methods.join(', ')}], host actions: [${actions.join(', ')}]` +
                      (actions.length ? ' (mark it actsOnHost)' : ''));
            }
        }
        check('the inventory covered the registry', tools.length > 90, `${tools.length} tools listed`);
        await mcp.stop();
    }

    // The editor's class table against the registry.
    {
        const tableSource = fs.readFileSync(path.join(repoRoot, 'Packages', 'ai-assistant', 'Editor', 'AssistantTools.cpp'), 'utf8');
        const rows = [...tableSource.matchAll(/\{"([a-z_0-9]+)", "([a-z_0-9]*)", Class::(\w+), "/g)]
            .map(m => ({ name: m[1], method: m[2], cls: m[3] }));
        const registry = (await import(pathToFileURL(path.join(repoRoot, 'mcp', 'dist', 'tools', 'index.js')).href)).allTools;
        const byName = new Map(registry.map(t => [t.name, t]));
        check('the class table has rows', rows.length > 90, `${rows.length} rows read`);
        const rowNames = new Set(rows.map(r => r.name));
        const missingRows = registry.filter(t => !rowNames.has(t.name)).map(t => t.name);
        check('every server tool has a class row', missingRows.length === 0, `no row: ${missingRows.join(', ')}`);
        const runsWithoutAsking = new Set(['Read', 'View', 'UndoableEdit', 'Input']);
        for (const row of rows) {
            const tool = byName.get(row.name);
            if (!tool) { check(`row ${row.name} names a server tool`, false, 'no such tool in mcp/src/tools'); continue; }
            // A tool that drives a method without proxying it names it in coversIpcMethods.
            const sent = tool.ipcMethod ?? tool.coversIpcMethods?.[0] ?? '';
            check(`row ${row.name} sends the tool's method`, row.method === sent, `row "${row.method}", tool "${sent}"`);
            if (row.method === '' && row.cls === 'Gated')
                check(`${row.name} runs in the server and is Gated, so it is actsOnHost`, tool.actsOnHost === true,
                      'mark it actsOnHost in mcp/src/tools, or it runs without asking the editor');
            if (tool.actsOnHost)
                check(`${row.name} acts on the machine, so its class asks or refuses`, !runsWithoutAsking.has(row.cls),
                      `class ${row.cls}`);
        }
    }

    // The project root bounds the host-side tools: GE_PROJECT_ROOT.
    {
        const project = fs.mkdtempSync(path.join(os.tmpdir(), 'assistant-check-project-'));
        const outside = fs.mkdtempSync(path.join(os.tmpdir(), 'assistant-check-outside-'));
        const actions = path.join(project, 'Assets', 'Scripts', 'GameGraphActions');
        fs.mkdirSync(actions, { recursive: true });
        fs.writeFileSync(path.join(actions, 'Existing.cpp'), '// kept');
        editor.admitAuthorize = true;
        const mcp = new McpProcess({ ...assistantEnv, GE_PROJECT_ROOT: project });
        await mcp.initialize();
        const call = async (name, args) => {
            const pathsBefore = mcp.hostPaths().length;
            const answer = await mcp.request('tools/call', { name, arguments: args });
            return { text: textOf(answer), isError: !!answer.result?.isError, paths: mcp.hostPaths().slice(pathsBefore) };
        };
        const inside = (p) => !path.relative(project, p).startsWith('..') && !path.isAbsolute(path.relative(project, p));

        const absolute = await call('create_game_system', { name: 'Waves', directory: outside });
        check('an absolute directory outside the project is refused', absolute.isError &&
              absolute.text.includes('outside the project') && absolute.paths.length === 0,
              `${absolute.text} | writes: [${absolute.paths.join(', ')}]`);
        const climbing = await call('get_game_graph', { path: '../escape.graph' });
        check('a relative path that leaves the project is refused, reads included', climbing.isError &&
              climbing.text.includes('outside the project'), climbing.text);
        const defaulted = await call('create_game_system', { name: 'Waves' });
        check('a default directory is inside the project', defaulted.paths.length > 0 && inside(defaulted.paths[0]),
              `writes: [${defaulted.paths.join(', ')}] (project ${project})`);
        const relative = await call('write_game_graph_action_code', { typeId: 'Fresh', code: '', outputDir: 'Assets/Generated' });
        check('a relative directory resolves against the project', relative.paths.length > 0 && inside(relative.paths[0]),
              `writes: [${relative.paths.join(', ')}] (project ${project})`);
        const kept = await call('write_game_graph_action_code', { typeId: 'Existing', code: '' });
        check('an existing action is not replaced without overwrite', kept.isError && kept.text.includes('overwrite') &&
              kept.paths.length === 0, `${kept.text} | writes: [${kept.paths.join(', ')}]`);
        const replaced = await call('write_game_graph_action_code', { typeId: 'Existing', code: '', overwrite: true });
        check('overwrite replaces an existing action', replaced.paths.length > 0 && inside(replaced.paths[0]),
              `${replaced.text} | writes: [${replaced.paths.join(', ')}]`);
        await mcp.stop();
        editor.admitAuthorize = false;
        fs.rmSync(project, { recursive: true, force: true });
        fs.rmSync(outside, { recursive: true, force: true });
    }

    // An empty GE_PROJECT_ROOT (no project open) refuses the content tools instead of
    // falling back to the repository.
    {
        editor.admitAuthorize = true;
        const mcp = new McpProcess({ ...assistantEnv, GE_PROJECT_ROOT: '' });
        await mcp.initialize();
        for (const [name, args] of [['create_game_system', { name: 'Waves' }], ['get_game_graph', { path: 'Graphs/a.graph' }]]) {
            const answer = await mcp.request('tools/call', { name, arguments: args });
            check(`${name} with no project open is refused`, answer.result?.isError && textOf(answer).includes('No project is open'),
                  textOf(answer) || JSON.stringify(answer));
        }
        check('no project open writes nothing', mcp.hostPaths().length === 0, `writes: [${mcp.hostPaths().join(', ')}]`);
        await mcp.stop();
        editor.admitAuthorize = false;
    }

    editor.server.close();
    fs.rmSync(radeonSuite, { recursive: true, force: true });
} catch (err) {
    console.error(`harness failure: ${err?.stack ?? err}`);
    process.exit(2);
}

if (failures.length > 0) {
    console.error(`MCP assistant check FAILED (${failures.length}):\n  ${failures.join('\n  ')}`);
    process.exit(1);
}
console.log('MCP assistant check OK: filter, start refusal, binding, refused binding, host-action inventory, class table, project root');
