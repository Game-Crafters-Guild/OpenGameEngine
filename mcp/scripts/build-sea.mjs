#!/usr/bin/env node
// Package the CLI as a standalone executable (Node Single Executable
// Application): esbuild-bundle dist/cli.js into one CommonJS file, embed it as
// a SEA blob in a copy of the running node binary, and smoke-test the result.
// The output needs no node, npm, or mcp/ checkout at runtime — it speaks TCP
// to the editor's debug server directly.
//
//   node scripts/build-sea.mjs --out-dir <dir> [--name gameenginecli]
//
// SEA does not cross-compile: each platform's build produces its own binary,
// the same way the engine itself does. macOS is re-signed ad hoc (SEA
// injection invalidates the existing signature and unsigned binaries do not
// run on arm64); on Windows the copied node.exe's Authenticode signature is
// left invalid — cosmetic for a local tool, re-sign before shipping externally.

import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const mcpDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

function arg(flag, fallback) {
  const i = process.argv.indexOf(flag);
  return i >= 0 ? process.argv[i + 1] : fallback;
}

const outDir = arg("--out-dir", path.join(mcpDir, "dist-exe"));
const baseName = arg("--name", "gameenginecli");
const exeName = process.platform === "win32" ? `${baseName}.exe` : baseName;
const exePath = path.join(outDir, exeName);

const cliEntry = path.join(mcpDir, "dist", "cli.js");
if (!fs.existsSync(cliEntry)) {
  console.error(`${cliEntry} is missing — run 'npm run build' first (the CMake target does).`);
  process.exit(1);
}

fs.mkdirSync(outDir, { recursive: true });
const workDir = fs.mkdtempSync(path.join(os.tmpdir(), "ge-sea-"));

try {
  // 1. One CommonJS file. SEA requires CJS; the sources are ESM and use
  // import.meta.url (paths.ts anchors repo-root discovery on it), so it is
  // rewritten to the executable's own path — which is exactly the anchor the
  // packaged binary should use.
  const esbuild = await import("esbuild");
  const bundlePath = path.join(workDir, "cli-bundle.cjs");
  await esbuild.build({
    entryPoints: [cliEntry],
    bundle: true,
    platform: "node",
    format: "cjs",
    outfile: bundlePath,
    define: { "import.meta.url": "__geImportMetaUrl" },
    banner: { js: "const __geImportMetaUrl = require('node:url').pathToFileURL(process.execPath).href;" },
    logLevel: "warning",
  });

  // 2. SEA preparation blob.
  const blobPath = path.join(workDir, "sea-prep.blob");
  const seaConfigPath = path.join(workDir, "sea-config.json");
  fs.writeFileSync(seaConfigPath, JSON.stringify({
    main: bundlePath,
    output: blobPath,
    disableExperimentalSEAWarning: true,
  }));
  execFileSync(process.execPath, ["--experimental-sea-config", seaConfigPath], { stdio: ["ignore", "inherit", "inherit"] });

  // 3. Copy the running node binary and inject the blob — in the work dir,
  // not the staged path: a failure here or in the smoke test must not have
  // clobbered a previous good exe with a half-injected impostor (a plain
  // renamed node.exe passes every "file exists" check and opens a REPL).
  const workExe = path.join(workDir, exeName);
  fs.copyFileSync(process.execPath, workExe);
  fs.chmodSync(workExe, 0o755);
  if (process.platform === "darwin") {
    execFileSync("codesign", ["--remove-signature", workExe], { stdio: "inherit" });
  }
  const { inject } = await import("postject");
  await inject(workExe, "NODE_SEA_BLOB", fs.readFileSync(blobPath), {
    sentinelFuse: "NODE_SEA_FUSE_fce680ab2cc467b6e072b8b5df1996b2",
    ...(process.platform === "darwin" ? { machoSegmentName: "NODE_SEA" } : {}),
  });
  if (process.platform === "darwin") {
    execFileSync("codesign", ["--sign", "-", workExe], { stdio: "inherit" });
  }

  // 4. Smoke-test what was just built — a packaging step that can silently
  // produce a broken binary must fail its own build instead. The tool list
  // must match dist/cli.js exactly: a floor would quietly tolerate a bundle
  // that loads but drops tools, should registration ever become dynamic.
  const listOf = (cmd, args) => {
    const r = spawnSync(cmd, args, { encoding: "utf8", timeout: 30_000 });
    if (r.status !== 0) {
      console.error(`Smoke test failed: '${[cmd, ...args].join(" ")}' exited ${r.status}.`);
      console.error(r.stderr ?? "");
      process.exit(1);
    }
    return (r.stdout ?? "").split("\n").filter((l) => l.trim()).sort();
  };
  const exeTools = listOf(workExe, ["list"]);
  const refTools = listOf(process.execPath, [cliEntry, "list"]);
  if (exeTools.length === 0 || exeTools.join("\n") !== refTools.join("\n")) {
    console.error(`Smoke test failed: exe lists ${exeTools.length} tools, dist/cli.js lists ${refTools.length}.`);
    process.exit(1);
  }

  // 5. Stage only a verified binary; rename over the old one is atomic on the
  // same volume, so no reader ever sees a partial file.
  const stagePath = `${exePath}.staging`;
  fs.copyFileSync(workExe, stagePath);
  fs.chmodSync(stagePath, 0o755);
  fs.renameSync(stagePath, exePath);
  const sizeMb = (fs.statSync(exePath).size / (1024 * 1024)).toFixed(0);
  console.log(`${exePath} — ${sizeMb} MB, ${exeTools.length} tools, smoke test OK`);
} finally {
  fs.rmSync(workDir, { recursive: true, force: true });
}
