"""
Verify the join key the whole anchoring design depends on:
    sidecar.replayId (Stats API MatchGuid)  ==  .replay header MatchGuid

If these never match, ReplayLocator.FindByMatchGuid always fails, the
post-match linker never anchors, and playback falls back to broken
wall-clock stamps — independent of the per-goal anchoring fix.

Prints, for every sidecar: its replayId, whether a .replay with that exact
MatchGuid exists, and (if not) the closest candidates by mtime so we can see
whether the .replay header simply stores a different GUID form.

Usage:  py tools/check_guid_join.py
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from replay_inspect import parse_replay  # noqa: E402

RECORDINGS = Path(os.environ.get("APPDATA", "")) / "SyncComms" / "recordings"
DEMOS = Path.home() / "Documents" / "My Games" / "Rocket League" / "TAGame" / "Demos"


def replay_guids() -> dict[str, str]:
    """Map lowercased .replay-header MatchGuid -> filename, for all parseable."""
    out: dict[str, str] = {}
    parsed = failed = 0
    for p in DEMOS.glob("*.replay"):
        try:
            r = parse_replay(p)
            g = (r["props"].get("MatchGuid", (None, ""))[1] or "")
            parsed += 1
            if g:
                out[g.lower()] = p.name
        except Exception:
            failed += 1
    print(f"parsed {parsed} .replay headers ({failed} failed), "
          f"{len(out)} with a non-empty MatchGuid\n")
    return out


def main() -> int:
    if not RECORDINGS.exists():
        print(f"no recordings dir: {RECORDINGS}")
        return 1
    guid_map = replay_guids()

    # Also collect the set of Id/Name props in case MatchGuid lives elsewhere.
    sidecars = sorted(RECORDINGS.glob("*_synccomms.json"))
    print(f"{len(sidecars)} sidecars\n")
    hits = 0
    for sc_path in sidecars:
        sc = json.loads(sc_path.read_text(encoding="utf-8"))
        rid = (sc.get("replayId") or "").lower()
        match = guid_map.get(rid)
        if match:
            hits += 1
            print(f"  MATCH   {rid}  ->  {match}")
        else:
            print(f"  NO JOIN {rid}  (not found among {len(guid_map)} replay GUIDs)")
    print(f"\n{hits}/{len(sidecars)} sidecars join to a .replay by MatchGuid")

    # Show a few sample replay GUIDs so we can eyeball the format difference.
    print("\nsample .replay header MatchGuids:")
    for g, name in list(guid_map.items())[:5]:
        print(f"  {g}   ({name})")
    print("\nsample sidecar replayIds:")
    for sc_path in sidecars[:5]:
        sc = json.loads(sc_path.read_text(encoding="utf-8"))
        print(f"  {(sc.get('replayId') or '').lower()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
