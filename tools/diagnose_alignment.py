"""
Diagnose audio-sync alignment for SyncComms recordings.

For each sidecar in %APPDATA%\\SyncComms\\recordings\\:
  - Read its captured segment stamps + anchored flag
  - Try to find the matching .replay in RL's Demos folder by MatchGuid
  - If found, parse goalFrames, compute the single-offset model, print residuals
  - Flag inconsistencies (per-segment offset variance, audio-duration mismatch,
    missing .replay, missing anchoring, etc.)

Output is plain-text, designed to be diff-friendly across runs.

Usage:
    py tools/diagnose_alignment.py                    # all sidecars
    py tools/diagnose_alignment.py <sidecar.json>     # one specific
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

# Reuse the .replay header parser
sys.path.insert(0, str(Path(__file__).parent))
from replay_inspect import parse_replay  # noqa: E402


FPS = 30.0
RECORDINGS_DIR = Path(os.environ.get("APPDATA", "")) / "SyncComms" / "recordings"
DEMOS_DIR = Path.home() / "Documents" / "My Games" / "Rocket League" / "TAGame" / "Demos"


def list_sidecars() -> list[Path]:
    if not RECORDINGS_DIR.exists():
        return []
    return sorted(RECORDINGS_DIR.glob("*_synccomms.json"),
                  key=lambda p: p.stat().st_mtime)


def find_replay_by_guid(guid: str) -> Path | None:
    """Same scan-and-parse approach as ReplayLocator. Quietly skips files that
    fail to parse (RL replays from very old engine versions, corrupt headers)."""
    if not DEMOS_DIR.exists():
        return None
    g = guid.lower()
    candidates = sorted(
        (p for p in DEMOS_DIR.glob("*.replay") if p.is_file()),
        key=lambda p: p.stat().st_mtime, reverse=True,  # newest first
    )
    for p in candidates:
        try:
            r = parse_replay(p)
            replay_guid = (r["props"].get("MatchGuid", (None, ""))[1] or "").lower()
            if replay_guid == g:
                return p
        except Exception:
            continue
    return None


def diagnose(sidecar_path: Path) -> None:
    print(f"\n{'=' * 78}")
    print(f"sidecar: {sidecar_path.name}  ({sidecar_path.stat().st_size:,} bytes)")
    print(f"{'=' * 78}")

    with sidecar_path.open(encoding="utf-8") as f:
        try:
            sc = json.load(f)
        except json.JSONDecodeError as e:
            print(f"  !! could not parse sidecar JSON: {e}")
            return

    replay_id = sc.get("replayId", "")
    anchored = sc.get("anchored", False)
    replay_path_field = sc.get("replayPath", "") or ""
    sample_rate = sc.get("sampleRate", 48000)
    channels = sc.get("channels", 1)
    segments = sc.get("segments", [])

    print(f"  replayId       : {replay_id}")
    print(f"  anchored       : {anchored}  {'<- post-match linker DID run' if anchored else '<- post-match linker did NOT run (no .replay or linker failed)'}")
    print(f"  replayPath     : {replay_path_field or '(empty)'}")
    print(f"  sampleRate     : {sample_rate}")
    print(f"  channels       : {channels}")
    print(f"  segments       : {len(segments)}")
    print(f"  captureDate    : {sc.get('captureDate', '?')}")

    if not segments:
        print("  (no segments — nothing to diagnose)")
        return

    # Sidecar segment stamps as stored on disk
    print()
    print(f"  Stamped segment times (as in sidecar):")
    print(f"    {'idx':>3} {'startTime':>11} {'endTime':>11} {'duration':>10} {'audioBytes':>11}")
    for s in segments:
        print(f"    {s.get('index', '?'):>3} {s.get('startTimeSec', 0):>11.4f} {s.get('endTimeSec', 0):>11.4f} "
              f"{s.get('endTimeSec', 0) - s.get('startTimeSec', 0):>10.4f} "
              f"{len(s.get('audioData', '') or ''):>11}")

    # Try to locate the .replay file
    print()
    replay_path: Path | None = None
    if replay_path_field and Path(replay_path_field).exists():
        replay_path = Path(replay_path_field)
        print(f"  .replay (from sidecar field): {replay_path}")
    else:
        replay_path = find_replay_by_guid(replay_id)
        if replay_path is not None:
            print(f"  .replay (found by GUID scan): {replay_path}")
        else:
            print(f"  .replay        : NOT FOUND in {DEMOS_DIR}")
            print(f"  ⚠ Without a .replay file the post-match linker cannot anchor.")
            print(f"  ⚠ Enable RL Settings > Replay > Auto-Save (or press 'Save Replay'")
            print(f"  ⚠ at end-of-match) so future matches produce a .replay we can use.")
            return

    # Parse the .replay header
    try:
        r = parse_replay(replay_path)
    except Exception as e:
        print(f"  !! .replay parse failed: {e}")
        return

    props = r["props"]
    num_frames = props.get("NumFrames", (None, 0))[1] or 0
    goals = props.get("Goals", (None, []))[1] or []
    goal_frames: list[int] = []
    for g in goals:
        entry = g.get("frame") or g.get("Frame")
        if entry and entry[1] is not None:
            goal_frames.append(int(entry[1]))
    goal_frames.sort()

    print(f"  numFrames      : {num_frames}  ({num_frames / FPS:.2f}s @ 30fps)")
    print(f"  goalFrames     : {goal_frames}")
    print(f"  goalSeconds    : {[round(f / FPS, 4) for f in goal_frames]}")

    if not goal_frames:
        print("  (no goals in .replay header — nothing to anchor against)")
        return

    # Per-segment offset analysis (assumes 1 segment per goal, BakkesMod-style)
    print()
    print(f"  Per-segment offset analysis (offset[i] = goalFrames[i]/30 − seg[i].endTime):")
    print(f"    {'i':>2} {'seg.endTime':>12} {'goal/30':>10} {'offset':>10}")

    n = min(len(segments), len(goal_frames))
    per_seg_offsets: list[float] = []
    for i in range(n):
        seg_end = segments[i].get("endTimeSec", 0.0)
        goal_sec = goal_frames[i] / FPS
        offset = goal_sec - seg_end
        per_seg_offsets.append(offset)
        print(f"    {i:>2} {seg_end:>12.4f} {goal_sec:>10.4f} {offset:>+10.4f}")

    if len(per_seg_offsets) < 2:
        print("\n  (only one segment — can't measure per-segment offset variance)")
        return

    spread = max(per_seg_offsets) - min(per_seg_offsets)
    print()
    print(f"  Offset spread (max − min): {spread:+.4f}s")
    if spread < 0.05:
        print(f"  ✓ offsets are consistent. Single-offset model is valid.")
    elif spread < 0.5:
        print(f"  ~ small drift across segments ({spread:.3f}s). Likely Stats-API jitter or per-segment WASAPI start latency. Tolerable.")
    else:
        print(f"  ⚠ LARGE drift across segments ({spread:.3f}s). Capture vs replay clocks are ticking at materially different rates, OR captured timestamps are not aligned with audio file boundaries.")

    # Diagnose linear drift specifically (sample-rate skew vs random jitter)
    if len(per_seg_offsets) >= 3:
        # Linear regression: how much does offset change per goal?
        deltas = [per_seg_offsets[i + 1] - per_seg_offsets[i]
                  for i in range(len(per_seg_offsets) - 1)]
        avg_delta = sum(deltas) / len(deltas)
        delta_spread = max(deltas) - min(deltas)
        print(f"  Inter-segment offset deltas: {[round(d, 4) for d in deltas]}")
        print(f"    average step: {avg_delta:+.4f}s/segment  (consistent step → linear drift; jitter → random)")
        print(f"    step spread : {delta_spread:.4f}s")
        if abs(avg_delta) > 0.1 and delta_spread < abs(avg_delta) * 0.5:
            print(f"    → dominantly linear: capture clock is running {'faster' if avg_delta < 0 else 'slower'} than replay clock")

    # Single-offset model (what the new code does)
    print()
    print(f"  Single-offset model (anchored to seg[0]):")
    base_offset = per_seg_offsets[0]
    print(f"    replayOffsetSec = {base_offset:+.4f}")
    print(f"    Anchored end-times if we apply this offset to all segments:")
    print(f"    {'i':>2} {'old.end':>10} {'new.end':>10} {'goal/30':>10} {'residual':>10}")
    for i in range(n):
        seg_end = segments[i].get("endTimeSec", 0.0)
        new_end = seg_end + base_offset
        goal_sec = goal_frames[i] / FPS
        residual = new_end - goal_sec
        print(f"    {i:>2} {seg_end:>10.4f} {new_end:>10.4f} {goal_sec:>10.4f} {residual:>+10.4f}")


def main(argv: list[str]) -> int:
    if len(argv) > 1:
        for arg in argv[1:]:
            p = Path(arg)
            if not p.exists():
                print(f"!! not found: {p}", file=sys.stderr)
                continue
            diagnose(p)
        return 0

    sidecars = list_sidecars()
    if not sidecars:
        print(f"No sidecars found in {RECORDINGS_DIR}")
        return 1
    print(f"Found {len(sidecars)} sidecar(s) in {RECORDINGS_DIR}")
    for p in sidecars:
        diagnose(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
