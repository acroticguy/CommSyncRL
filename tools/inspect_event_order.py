"""
Deep inspection of recorded Stats API streams around goal events.

Answers three questions the timing fix depends on:
  Q1. Does GoalScored fire more than once per goal (re-fire during the
      in-match cinematic)? Exact wire ordering vs GoalReplayStart/End,
      CountdownBegin, and bReplay flips.
  Q2. Which lifecycle events actually appear on the wire (is GoalReplayStart
      reliable as the segment-close hook)?
  Q3. Clean Frame-rate measurement during strictly-advancing replay playback,
      and Elapsed-vs-Frame behavior excluding scrubs.

Usage:  py tools/inspect_event_order.py [tools/stats_api_raw_*.jsonl ...]
"""

from __future__ import annotations

import json
import sys
from collections import Counter
from datetime import datetime
from glob import glob
from pathlib import Path

FPS = 30.0


def wall(ts: str) -> float:
    return datetime.fromisoformat(ts).timestamp()


def analyze(path: Path) -> None:
    print(f"\n{'=' * 78}\nstream: {path.name}\n{'=' * 78}")

    counts: Counter[str] = Counter()
    timeline: list[tuple[float, str, str]] = []  # (wall, label, guid) non-tick events
    prev_state: dict[str, tuple[bool, object]] = {}  # guid -> (bReplay, frame)
    ticks: list[tuple[float, object, object, bool, str]] = []  # (w, frame, elapsed, bReplay, guid)
    t0: float | None = None

    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            t = msg.get("t")
            if not t:
                continue
            w = wall(t)
            if t0 is None:
                t0 = w
            ev = msg.get("event", "")
            data = msg.get("data") or {}
            guid = (data.get("MatchGuid") or "")[:8]
            counts[ev] += 1

            if ev in ("UpdateState", "GameState"):
                game = data.get("Game") or {}
                br = bool(game.get("bReplay", False))
                fr = game.get("Frame")
                el = game.get("Elapsed")
                ticks.append((w, fr, el, br, guid))
                prev = prev_state.get(guid)
                if prev is None or prev[0] != br or (prev[1] is None) != (fr is None):
                    timeline.append(
                        (w, f"UpdateState bReplay={br} Frame={'yes' if fr is not None else 'no'}", guid))
                prev_state[guid] = (br, fr)
            else:
                extra = ""
                if ev == "GoalScored":
                    extra = f" GoalTime={data.get('GoalTime')}" \
                            f" Scorer={(data.get('Scorer') or {}).get('Name')}"
                timeline.append((w, ev + extra, guid))

    print("\nQ2  event counts on the wire:")
    for ev, c in sorted(counts.items()):
        print(f"      {ev:<26} {c}")

    print("\nQ1  non-tick timeline (and bReplay/Frame transitions), t=0 at stream start:")
    for w, label, guid in timeline:
        print(f"      t={w - t0:9.3f}s  [{guid}] {label}")

    # Q3: clean playback-rate measurement per replay session
    print("\nQ3  replay-viewer Frame behavior (strictly advancing stretches only):")
    by_guid: dict[str, list[tuple[float, int, float]]] = {}
    for w, fr, el, br, guid in ticks:
        if br and isinstance(fr, (int, float)):
            by_guid.setdefault(guid, []).append((w, int(fr), float(el or 0.0)))
    for guid, rows in by_guid.items():
        rows.sort()
        rates = []
        el_vs_fr = []
        for a, b in zip(rows, rows[1:]):
            dw, df, de = b[0] - a[0], b[1] - a[1], b[2] - a[2]
            if 0.01 < dw < 0.5 and 0 < df <= 30:        # advancing, no scrub
                rates.append(df / dw)
                el_vs_fr.append((df / FPS, de))
        if not rates:
            print(f"      [{guid}] no advancing stretches")
            continue
        rates.sort()
        med = rates[len(rates) // 2]
        frame_sec = sum(p[0] for p in el_vs_fr)
        elapsed_sec = sum(p[1] for p in el_vs_fr)
        print(f"      [{guid}] median rate={med:6.2f} f/s over {len(rates)} advancing ticks; "
              f"sum dFrame/30={frame_sec:7.2f}s vs sum dElapsed={elapsed_sec:7.2f}s "
              f"(ratio {elapsed_sec / frame_sec if frame_sec else 0:.3f})")


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv[1:]] or \
            [Path(p) for p in sorted(glob("tools/stats_api_raw_*.jsonl"))]
    for p in paths:
        analyze(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
