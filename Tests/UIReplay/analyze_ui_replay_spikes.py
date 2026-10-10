import argparse
import json
import os


def iter_frames(path: str):
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            try:
                j = json.loads(line)
            except Exception:
                continue
            if j.get("kind") != "ui_replay_frame":
                continue
            yield j


def main():
    ap = argparse.ArgumentParser(description="Summarize slow frames in a UIReplay JSONL log.")
    ap.add_argument("log", help="Path to .jsonl file")
    ap.add_argument("--threshold-ms", type=float, default=4.0, help="Report frames with totalMs >= this")
    ap.add_argument("--top", type=int, default=30, help="Also print top-N frames by totalMs")
    ap.add_argument("--frame-start", type=int, default=0, help="Only consider frames >= this")
    ap.add_argument("--frame-end", type=int, default=-1, help="Only consider frames <= this (or <0 for no limit)")
    ap.add_argument("--exclude-heavy", action="store_true", help="Exclude frames where uiDecision.ranHeavyPass is true")
    ap.add_argument(
        "--exclude-request-relayout",
        action="store_true",
        help="Exclude frames where uiDecision.requestRelayout is true",
    )
    ap.add_argument("--only-dirty-now-zero", action="store_true", help="Only include frames where uiDecision.dirtyNow == 0")
    args = ap.parse_args()

    if not os.path.exists(args.log):
        raise SystemExit(f"missing log: {args.log}")

    rows = []
    for j in iter_frames(args.log):
        ui = j.get("ui") or {}
        total = float(ui.get("totalMs", 0.0) or 0.0)
        frame = int(j.get("frame", -1))
        dec = j.get("uiDecision") or {}
        if frame < args.frame_start:
            continue
        if args.frame_end >= 0 and frame > args.frame_end:
            continue
        if args.exclude_heavy and bool(dec.get("ranHeavyPass")):
            continue
        if args.exclude_request_relayout and bool(dec.get("requestRelayout")):
            continue
        if args.only_dirty_now_zero and int(dec.get("dirtyNow", 0) or 0) != 0:
            continue
        rows.append((total, frame, ui, dec))

    rows_sorted = sorted(rows, key=lambda t: t[0], reverse=True)
    top_n = rows_sorted[: max(0, args.top)]

    def fmt(v):
        try:
            return f"{float(v):.3f}"
        except Exception:
            return "0.000"

    print(f"log={args.log}")
    print(f"frames={len(rows)} thresholdMs={args.threshold_ms:.3f}")

    slow = [r for r in rows if r[0] >= args.threshold_ms]
    print(f"slowFrames(count)={len(slow)}")

    if slow:
        print("\n## Slow frames (>= threshold)")
        for total, frame, ui, dec in sorted(slow, key=lambda t: t[0], reverse=True):
            print(
                f"frame={frame} totalMs={fmt(total)} buildYogaMs={fmt(ui.get('buildYogaMs'))} "
                f"yogaMs={fmt(ui.get('yogaMs'))} geometryMs={fmt(ui.get('geometryMs'))} "
                f"hitTestMs={fmt(ui.get('hitTestMs'))} eventDispatchMs={fmt(ui.get('eventDispatchMs'))} "
                f"ranHeavy={bool(dec.get('ranHeavyPass'))} dirtyNow={int(dec.get('dirtyNow', 0) or 0)} "
                f"requestRelayout={bool(dec.get('requestRelayout'))} "
                f"virtDrainCalls={int(dec.get('virtualizationDrainCalls', 0) or 0)} "
                f"virtDrainIters={int(dec.get('virtualizationDrainIterations', 0) or 0)} "
                f"pendingInputs={int(dec.get('pendingInputs', 0) or 0)} "
                f"pendingKeys={int(dec.get('pendingKeyEvents', 0) or 0)}"
            )

    if top_n:
        print("\n## Top frames by totalMs")
        for total, frame, ui, dec in top_n:
            print(
                f"frame={frame} totalMs={fmt(total)} buildYogaMs={fmt(ui.get('buildYogaMs'))} "
                f"yogaMs={fmt(ui.get('yogaMs'))} geometryMs={fmt(ui.get('geometryMs'))} "
                f"ranHeavy={bool(dec.get('ranHeavyPass'))} "
                f"dirtyNow={int(dec.get('dirtyNow', 0) or 0)}"
            )


if __name__ == "__main__":
    main()

