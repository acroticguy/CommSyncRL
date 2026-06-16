"""
Validate the per-goal anchoring timing model against recorded Stats API
streams (tools/stats_api_raw_*.jsonl, written by stats_api_probe.py).

The fix in SegmentAnchoring.cpp rests on these empirical claims:

  C1. During LIVE play there is no Frame/Elapsed — capture must use wall-clock.
  C2. During REPLAY viewing, Frame advances ~30 frames per wall-second
      (monotonic except scrubs), so Frame/30 is a wall-clock-rate timeline.
  C3. Elapsed (in-game clock) stalls during cinematics/countdowns while Frame
      keeps advancing — Frame is the right sync source.
  C4. The replay timeline EXCLUDES live goal-cinematic time, so live
      wall-clock and replay Frame/30 diverge by several seconds per goal —
      one match-wide offset cannot align more than one segment.
  C5. GoalReplayStart (segment close) lags GoalScored (the goal) by seconds —
      anchoring on segment END bakes in that bias; the GoalScored stamp is
      the correct anchor point.

For C4 we compare, per MatchGuid present in BOTH phases of a stream:
  live:   wall-time gaps between consecutive GoalScored events
  replay: Frame/30 gaps between consecutive score-change ticks
The per-gap difference is the cinematic dead time a single offset would
misalign — expected >> 0.2s (the playback segment-matching tolerance).

Usage:  py tools/validate_timing_model.py [tools/stats_api_raw_*.jsonl ...]
"""

from __future__ import annotations

import json
import sys
from datetime import datetime
from glob import glob
from pathlib import Path

FPS = 30.0


def wall(ts: str) -> float:
    return datetime.fromisoformat(ts).timestamp()


def analyze(path: Path) -> None:
    print(f"\n{'=' * 78}\nstream: {path.name}\n{'=' * 78}")

    # Per-guid live data
    live_goals: dict[str, list[float]] = {}        # GoalScored wall times
    live_replay_starts: dict[str, list[float]] = {}  # GoalReplayStart wall times
    live_countdowns: dict[str, list[float]] = {}
    live_tick_count = 0
    live_tick_with_frame = 0

    # Per-guid replay-viewing data: (wall, frame, elapsed, total_score)
    replay_ticks: dict[str, list[tuple[float, int, float, int]]] = {}

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
            ev = msg.get("event", "")
            data = msg.get("data") or {}
            if not t:
                continue
            w = wall(t)
            guid = data.get("MatchGuid", "")

            if ev == "GoalScored":
                live_goals.setdefault(guid, []).append(w)
            elif ev == "GoalReplayStart":
                live_replay_starts.setdefault(guid, []).append(w)
            elif ev == "CountdownBegin":
                live_countdowns.setdefault(guid, []).append(w)
            elif ev in ("UpdateState", "GameState"):
                game = data.get("Game") or {}
                breplay = bool(game.get("bReplay", False))
                frame = game.get("Frame")
                if breplay and isinstance(frame, (int, float)):
                    elapsed = float(game.get("Elapsed", 0.0))
                    score = sum(int(team.get("Score", 0))
                                for team in (game.get("Teams") or []))
                    replay_ticks.setdefault(guid, []).append(
                        (w, int(frame), elapsed, score))
                else:
                    live_tick_count += 1
                    if frame is not None:
                        live_tick_with_frame += 1

    # --- C1: live ticks carry no Frame ---
    print(f"\nC1  live UpdateState ticks: {live_tick_count}, "
          f"of which with Frame field: {live_tick_with_frame} "
          f"({'OK — capture cannot use Frame' if live_tick_with_frame == 0 else 'UNEXPECTED'})")

    # --- C5: GoalScored -> GoalReplayStart lag (celebration bias) ---
    print("\nC5  GoalScored -> GoalReplayStart lag (segment-end anchor bias):")
    any_pair = False
    for guid, goals in live_goals.items():
        starts = live_replay_starts.get(guid, [])
        for g in goals:
            after = [s for s in starts if s >= g]
            if after:
                any_pair = True
                print(f"      {guid[:8]}…  goal->cinematic: {after[0] - g:6.2f}s")
    if not any_pair:
        print("      (no GoalScored/GoalReplayStart pairs in this stream)")

    for guid, ticks in replay_ticks.items():
        if len(ticks) < 10:
            continue
        ticks.sort(key=lambda x: x[0])
        print(f"\n  replay-viewing session for {guid[:8]}…  ({len(ticks)} ticks)")

        # --- C2: Frame rate vs wall clock (excluding scrubs/stalls) ---
        fwd = [(b[0] - a[0], b[1] - a[1])
               for a, b in zip(ticks, ticks[1:])
               if 0 < b[0] - a[0] < 0.5 and 0 <= b[1] - a[1] <= 30]
        if fwd:
            dt = sum(p[0] for p in fwd)
            df = sum(p[1] for p in fwd)
            print(f"C2    frame rate over contiguous playback: {df / dt:.2f} frames/wall-sec"
                  f"  (claim: ~{FPS:.0f})")
        jumps = [(a[1], b[1]) for a, b in zip(ticks, ticks[1:])
                 if abs(b[1] - a[1]) > 60]
        if jumps:
            print(f"      scrub-like Frame jumps observed: "
                  f"{[(int(a), int(b)) for a, b in jumps[:6]]}")

        # --- C3: Elapsed stalls while Frame advances ---
        stall_frames = 0
        for a, b in zip(ticks, ticks[1:]):
            if b[1] > a[1] and abs(b[2] - a[2]) < 1e-6:
                stall_frames += b[1] - a[1]
        print(f"C3    frames advanced while Elapsed was frozen: {stall_frames} "
              f"({stall_frames / FPS:.1f}s of Frame motion with a stalled in-game clock)")
        print(f"      session start: Frame={ticks[0][1]}, Elapsed={ticks[0][2]:.2f} "
              f"(preroll offset between the two timelines)")

        # --- C4: goals in replay (score changes) vs live wall-clock gaps ---
        goal_frames: list[int] = []
        for a, b in zip(ticks, ticks[1:]):
            if b[3] > a[3] and abs(b[1] - a[1]) <= 30:  # ignore scrub re-plays
                goal_frames.append(b[1])
        live = sorted(live_goals.get(guid, []))
        print(f"C4    goals seen in replay stream (score changes): "
              f"frames {goal_frames}  -> sec {[round(f / FPS, 2) for f in goal_frames]}")
        if len(goal_frames) >= 2 and len(live) >= 2 and len(goal_frames) == len(live):
            print(f"      gap comparison (live wall vs replay Frame/30):")
            for i in range(1, len(live)):
                wall_gap = live[i] - live[i - 1]
                frame_gap = (goal_frames[i] - goal_frames[i - 1]) / FPS
                print(f"        goal{i - 1}->goal{i}: live={wall_gap:7.2f}s  "
                      f"replay={frame_gap:7.2f}s  divergence={wall_gap - frame_gap:+7.2f}s")
        elif live:
            print(f"      live GoalScored count={len(live)} vs replay score-changes="
                  f"{len(goal_frames)} — gap comparison "
                  f"{'skipped (count mismatch)' if goal_frames else 'not possible'}")
        else:
            print(f"      (no live-phase GoalScored for this guid in this stream)")


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv[1:]] or \
            [Path(p) for p in sorted(glob("tools/stats_api_raw_*.jsonl"))]
    if not paths:
        print("no stats_api_raw_*.jsonl found")
        return 1
    for p in paths:
        analyze(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
