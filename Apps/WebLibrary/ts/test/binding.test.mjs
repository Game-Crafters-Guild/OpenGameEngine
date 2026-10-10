// The binding's engine pack join (opengine-core-binding.js): a folder that serves the pack as
// parts with an index gets one Blob of the parts in order; a folder without the index serves the
// pack whole; a missing, resized or damaged part fails naming what does not match.
import assert from 'node:assert/strict';
import http from 'node:http';
import test from 'node:test';
import { joinEnginePack } from '../../opengine-core-binding.js';

/** Serves `files` (name to bytes) on a local port; resolves to its base URL and closer. */
function serve(files) {
    const server = http.createServer((request, response) => {
        const body = files.get(new URL(request.url, 'http://host').pathname.slice(1));
        response.writeHead(body ? 200 : 404);
        response.end(body);
    });
    return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({
        base: `http://127.0.0.1:${server.address().port}/core/`,
        close: () => new Promise((done) => server.close(done)),
    })));
}

const kParts = [Buffer.from('GEPAK first part '), Buffer.from('second part '), Buffer.from('third')];

/** FNV-1a 64 as Tools/Web/gepak.py writes it into the index. */
function fnv1a64(bytes) {
    let hash = 0xcbf29ce484222325n;
    for (const byte of bytes) hash = ((hash ^ BigInt(byte)) * 0x100000001b3n) & 0xffffffffffffffffn;
    return `0x${hash.toString(16).padStart(16, '0')}`;
}

const kWhole = Buffer.concat(kParts);
const kIndex = {
    file: 'opengine-core.gepak', bytes: kWhole.length, fnv1a64: fnv1a64(kWhole),
    parts: kParts.map((bytes, i) => ({ file: `opengine-core.gepak.part${i}`, bytes: bytes.length })),
};

function partFiles(index = kIndex, parts = kParts) {
    const files = new Map([['core/opengine-core.gepak.parts.json', Buffer.from(JSON.stringify(index))]]);
    parts.forEach((bytes, i) => files.set(`core/opengine-core.gepak.part${i}`, bytes));
    return files;
}

async function joinRejects(files, message) {
    const site = await serve(files);
    try {
        await assert.rejects(joinEnginePack(new URL(site.base)), message);
    } finally {
        await site.close();
    }
}

test('a pack served as parts joins into one blob, in the index order', async () => {
    const site = await serve(partFiles());
    try {
        const url = await joinEnginePack(new URL(site.base));
        assert.match(url, /^blob:/);
        const joined = Buffer.from(await (await fetch(url)).arrayBuffer());
        assert.deepEqual(joined, Buffer.concat(kParts));
        URL.revokeObjectURL(url);
    } finally {
        await site.close();
    }
});

test('a folder without the index serves the pack whole', async () => {
    const site = await serve(new Map());
    try {
        assert.equal(await joinEnginePack(new URL(site.base)), null);
    } finally {
        await site.close();
    }
});

test('a missing part fails naming the file', async () => {
    const files = partFiles();
    files.delete('core/opengine-core.gepak.part1');
    const site = await serve(files);
    try {
        await assert.rejects(joinEnginePack(new URL(site.base)), /Could not fetch opengine-core\.gepak\.part1 of the engine pack .*HTTP 404/);
    } finally {
        await site.close();
    }
});

test('a part of another size fails naming it', async () => {
    const parts = [kParts[0], Buffer.from('second part!!'), kParts[2]];
    await joinRejects(partFiles(kIndex, parts), /opengine-core\.gepak\.part1 of the engine pack is 13 bytes; the index says 12/);
});

test('a part of the right size with other bytes fails the pack checksum', async () => {
    const parts = [kParts[0], Buffer.from('SECOND part '), kParts[2]];
    await joinRejects(partFiles(kIndex, parts), /FNV-1a 0x[0-9a-f]{16}; the index says \d+ bytes and 0x[0-9a-f]{16}\. A part is stale or damaged/);
});
