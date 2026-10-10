#!/usr/bin/env node
"use strict";
//
// compare-baseline.js — canonical projection + comparison for UIReplay baselines.
//
// The ONLY fields that are stable across different Editor binaries are the input
// trajectory and the resulting UI focus/hover/capture state. Timings (buildYogaMs,
// totalMs, ...) and elementCount are binary- and machine-dependent and MUST NEVER
// be compared across binaries. This script projects each ui_replay_frame record
// onto that stable surface so sessions stop hand-rolling projections (and stop
// accidentally diffing timings).
//
// Standard projection (per frame):
//   frame, hoverId, focusId, mouse.x, mouse.y, capture.captured, capture.captureId
//
// Usage:
//   node compare-baseline.js <a.jsonl> <b.jsonl>     compare (exit 0 = IDENTICAL, 1 = DIFFS)
//   node compare-baseline.js --emit <in.jsonl> <out.jsonl>   write the projection of <in> to <out>
//
// The committed baseline under Tests/UIReplay/baselines/ is itself a projection
// file (produced with --emit); projecting an already-projected file is a no-op,
// so comparing a committed baseline against a fresh full run "just works".
//
const fs = require("fs");

const kMaxReportedDiffs = 40;

// Project one JSONL file to an array of canonical per-frame tuples, ordered by frame.
function projectFrames(path) {
  const text = fs.readFileSync(path, "utf8");
  const out = [];
  for (const rawLine of text.split("\n")) {
    const line = rawLine.trim();
    if (!line) continue;
    let rec;
    try {
      rec = JSON.parse(line);
    } catch (e) {
      continue; // tolerate partial/garbage lines
    }
    if (!rec || rec.kind !== "ui_replay_frame") continue;
    const mouse = rec.mouse || {};
    const cap = rec.capture || {};
    out.push({
      kind: "ui_replay_frame",
      frame: rec.frame,
      hoverId: rec.hoverId != null ? rec.hoverId : "",
      focusId: rec.focusId != null ? rec.focusId : "",
      mouse: { x: mouse.x != null ? mouse.x : 0, y: mouse.y != null ? mouse.y : 0 },
      capture: { captured: !!cap.captured, captureId: cap.captureId != null ? cap.captureId : "" },
    });
  }
  out.sort((a, b) => a.frame - b.frame);
  return out;
}

function fmt(p) {
  return `hover='${p.hoverId}' focus='${p.focusId}' mouse=(${p.mouse.x},${p.mouse.y}) ` +
         `captured=${p.capture.captured} captureId='${p.capture.captureId}'`;
}

function projectionsEqual(a, b) {
  return a.hoverId === b.hoverId &&
         a.focusId === b.focusId &&
         a.mouse.x === b.mouse.x &&
         a.mouse.y === b.mouse.y &&
         a.capture.captured === b.capture.captured &&
         a.capture.captureId === b.capture.captureId;
}

function emit(inPath, outPath) {
  const frames = projectFrames(inPath);
  fs.writeFileSync(outPath, frames.map((f) => JSON.stringify(f)).join("\n") + "\n", "utf8");
  console.log(`emitted ${frames.length} projected frames -> ${outPath}`);
}

function compare(aPath, bPath) {
  const a = projectFrames(aPath);
  const b = projectFrames(bPath);
  const byFrameB = new Map(b.map((p) => [p.frame, p]));
  const byFrameA = new Map(a.map((p) => [p.frame, p]));

  const diffs = [];
  if (a.length !== b.length) {
    diffs.push(`frame COUNT differs: A=${a.length} B=${b.length}`);
  }

  for (const pa of a) {
    const pb = byFrameB.get(pa.frame);
    if (!pb) {
      diffs.push(`frame ${pa.frame}: present in A, MISSING in B`);
      continue;
    }
    if (!projectionsEqual(pa, pb)) {
      diffs.push(`frame ${pa.frame}:\n    A: ${fmt(pa)}\n    B: ${fmt(pb)}`);
    }
  }
  for (const pb of b) {
    if (!byFrameA.has(pb.frame)) {
      diffs.push(`frame ${pb.frame}: present in B, MISSING in A`);
    }
  }

  console.log(`A: ${aPath}  (${a.length} frames)`);
  console.log(`B: ${bPath}  (${b.length} frames)`);
  console.log(`Projection: frame, hoverId, focusId, mouse.x/y, capture.captured/captureId`);
  console.log(`(timings + elementCount are deliberately NOT compared)`);

  if (diffs.length === 0) {
    console.log("\nIDENTICAL");
    return 0;
  }
  console.log(`\nDIFFS: ${diffs.length}`);
  for (const d of diffs.slice(0, kMaxReportedDiffs)) console.log("  " + d);
  if (diffs.length > kMaxReportedDiffs) {
    console.log(`  ... and ${diffs.length - kMaxReportedDiffs} more`);
  }
  return 1;
}

function main() {
  const args = process.argv.slice(2);
  if (args[0] === "--emit") {
    if (args.length !== 3) {
      console.error("usage: node compare-baseline.js --emit <in.jsonl> <out.jsonl>");
      process.exit(2);
    }
    emit(args[1], args[2]);
    process.exit(0);
  }
  if (args.length !== 2) {
    console.error("usage: node compare-baseline.js <a.jsonl> <b.jsonl>");
    console.error("       node compare-baseline.js --emit <in.jsonl> <out.jsonl>");
    process.exit(2);
  }
  process.exit(compare(args[0], args[1]));
}

main();
