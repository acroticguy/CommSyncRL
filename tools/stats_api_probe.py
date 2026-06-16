"""
Rocket League Stats API — pivot-events probe.

Filtered version that only reports the events the SyncComms standalone pivot
actually consumes. Everything else (BallHit, CrossbarHit, ClockUpdatedSeconds,
RoundStarted, non-Goal Statfeeds, PodiumStart, etc.) is dropped from stdout.

The raw .jsonl log still contains every event verbatim, so we can investigate
later if something unexpected comes up.

Events this script surfaces:
    MatchCreated          → pre-arm
    CountdownBegin        → arm recording
    MatchPaused/Unpaused  → pause-aware capture
    StatfeedEvent[Goal]   → goal segmentation  (other Statfeed Types ignored)
    MatchEnded            → finalize + compress + sidecar
    MatchDestroyed        → cleanup
    ReplayCreated         → .replay-on-disk → sidecar association
    UpdateState           → replay scrubbing sync, but only on:
                              - bReplay transitions
                              - detected scrubs (elapsed jumps > 1s)
                              - 5s heartbeat while in a match

Setup before running:
    1. Edit `<RL Install>\TAGame\Config\DefaultStatsAPI.ini`:
           PacketSendRate=30
           Port=49123
    2. Launch Rocket League.
    3. python tools/stats_api_probe.py

Output files (alongside the script unless --out-dir is given):
    stats_api_raw_<timestamp>.jsonl  — every event, full payload
    stats_api_replay_<timestamp>.csv — UpdateState samples for sync analysis

Stdlib only.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import socket
import sys
import time
from pathlib import Path


HOST = "127.0.0.1"
DEFAULT_PORT = 49123
RECONNECT_DELAY_S = 2.0
HEARTBEAT_S = 5.0
SCRUB_THRESHOLD_S = 1.0  # elapsed jump that signals a replay scrub


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--host", default=HOST)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--out-dir", default=".")
    return p.parse_args()


def stream_json_objects(sock: socket.socket):
    """Yield decoded JSON objects from a TCP stream, robust to whatever framing
    the API uses (newline-delimited, concatenated, or with stray prefix bytes)."""
    decoder = json.JSONDecoder()
    buf = ""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return
        buf += chunk.decode("utf-8", errors="replace")
        while True:
            buf = buf.lstrip()
            if not buf:
                break
            if buf[0] != "{":
                next_open = buf.find("{")
                if next_open < 0:
                    buf = ""
                    break
                buf = buf[next_open:]
            try:
                obj, idx = decoder.raw_decode(buf)
            except json.JSONDecodeError:
                break
            buf = buf[idx:]
            yield obj


def normalize_event(obj: dict) -> tuple[str, dict]:
    name = obj.get("event") or obj.get("Event") or obj.get("name") or "<unknown>"
    data = obj.get("data") if "data" in obj else obj.get("Data", {})
    if isinstance(data, str):
        try:
            data = json.loads(data)
        except json.JSONDecodeError:
            pass
    return str(name), data if isinstance(data, dict) else {"_raw": data}


def extract_state(data: dict) -> dict | None:
    """Return frame/elapsed/bReplay/MatchGuid from an UpdateState payload, or
    None if this isn't a real state sample."""
    if not isinstance(data, dict):
        return None
    game = data.get("game") or data.get("Game") or {}
    if not isinstance(game, dict):
        game = {}

    def first(d: dict, *keys):
        for k in keys:
            if k in d:
                return d[k]
        return None

    frame = first(game, "Frame", "frame")
    elapsed = first(game, "Elapsed", "elapsed")
    is_replay = first(game, "bReplay", "IsReplay", "isReplay")
    if frame is None and elapsed is None and is_replay is None:
        return None
    return {
        "frame": frame,
        "elapsed": elapsed,
        "bReplay": bool(is_replay) if is_replay is not None else None,
        "matchGuid": first(data, "MatchGuid", "matchGuid"),
    }


def short_guid(g: str | None) -> str:
    if not g:
        return "—"
    return g[:8] + "…"


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    raw_path = out_dir / f"stats_api_raw_{ts}.jsonl"
    csv_path = out_dir / f"stats_api_replay_{ts}.csv"

    print(f"[probe] raw log     : {raw_path}")
    print(f"[probe] replay CSV  : {csv_path}")
    print(f"[probe] connecting  : {args.host}:{args.port}")
    print(f"[probe] surfacing only pivot-relevant events; full stream still in raw log.")
    print()

    raw_f = raw_path.open("w", encoding="utf-8")
    csv_f = csv_path.open("w", encoding="utf-8", newline="")
    csv_w = csv.writer(csv_f)
    csv_w.writerow(["wall_time", "frame", "elapsed", "bReplay", "matchGuid"])

    last_state: dict | None = None
    last_heartbeat = 0.0

    try:
        while True:
            try:
                with socket.create_connection((args.host, args.port), timeout=10) as sock:
                    sock.settimeout(None)
                    print(f"[probe] connected. waiting for events...")
                    print(f"[probe] (no events in 10s? confirm DefaultStatsAPI.ini PacketSendRate>0 and restart RL)")
                    print()

                    for obj in stream_json_objects(sock):
                        wall = dt.datetime.now().isoformat(timespec="milliseconds")
                        name, data = normalize_event(obj)

                        # Always preserve the raw event for forensic use later.
                        raw_f.write(json.dumps({"t": wall, "event": name, "data": data}, default=str))
                        raw_f.write("\n")
                        raw_f.flush()

                        guid = data.get("MatchGuid") or data.get("matchGuid")

                        # ── Lifecycle: clean one-line prints ────────────────────────────
                        if name == "MatchCreated":
                            print(f"[{wall}] MatchCreated      match={short_guid(guid)}")
                            last_state = None
                            last_heartbeat = 0.0

                        elif name == "CountdownBegin":
                            print(f"[{wall}] CountdownBegin    match={short_guid(guid)}  ← arm recording")

                        elif name == "MatchPaused":
                            print(f"[{wall}] MatchPaused       match={short_guid(guid)}")

                        elif name == "MatchUnpaused":
                            print(f"[{wall}] MatchUnpaused     match={short_guid(guid)}")

                        elif name == "MatchEnded":
                            winner = data.get("WinnerTeamNum")
                            print(f"[{wall}] MatchEnded        match={short_guid(guid)}  winner_team={winner}  ← finalize")

                        elif name == "MatchDestroyed":
                            print(f"[{wall}] MatchDestroyed    match={short_guid(guid)}  ← cleanup")
                            last_state = None

                        elif name == "ReplayCreated":
                            print(f"[{wall}] ReplayCreated     match={short_guid(guid)}  ← .replay on disk, write sidecar")

                        # ── Goal segmentation: filter Statfeeds to Type==Goal only ──────
                        elif name == "StatfeedEvent":
                            stat_type = (data.get("Type") or data.get("EventName") or "").lower()
                            if stat_type == "goal":
                                main = data.get("MainTarget") or {}
                                secondary = data.get("SecondaryTarget") or {}
                                tag = (f"  assist={secondary.get('Name')!r}"
                                       if secondary.get("Name") else "")
                                print(f"[{wall}] >>> GOAL <<<      "
                                      f"by={main.get('Name')!r} team={main.get('TeamNum')}"
                                      f"{tag}  match={short_guid(guid)}  ← segment")
                            # All other Statfeed types (Shot on Goal, Save, Epic Save, Win,
                            # Demolish, Assist, etc.) intentionally suppressed.

                        # ── Replay sync: only print on transitions, scrubs, or heartbeat ─
                        elif name in ("UpdateState", "Update_State", "GameState"):
                            state = extract_state(data)
                            if state is None:
                                continue

                            csv_w.writerow([wall, state["frame"], state["elapsed"],
                                            state["bReplay"], state["matchGuid"]])
                            csv_f.flush()

                            now = time.monotonic()
                            reason: str | None = None

                            if last_state is None:
                                reason = "first sample"
                            elif state["bReplay"] != last_state["bReplay"]:
                                reason = ("entered replay" if state["bReplay"]
                                          else "exited replay")
                            elif (
                                state["bReplay"]
                                and state["elapsed"] is not None
                                and last_state["elapsed"] is not None
                                and abs(state["elapsed"] - last_state["elapsed"]) > SCRUB_THRESHOLD_S
                            ):
                                delta = state["elapsed"] - last_state["elapsed"]
                                reason = f"scrub Δ={delta:+.2f}s"
                            elif now - last_heartbeat >= HEARTBEAT_S:
                                reason = "heartbeat"

                            if reason is not None:
                                last_heartbeat = now
                                print(f"[{wall}] UpdateState       "
                                      f"bReplay={state['bReplay']}  "
                                      f"elapsed={state['elapsed']:.2f}  "
                                      f"frame={state['frame']}  "
                                      f"match={short_guid(state['matchGuid'])}  "
                                      f"({reason})")
                            last_state = state

                        # Everything else (BallHit, CrossbarHit, ClockUpdatedSeconds,
                        # MatchInitialized, RoundStarted, PodiumStart, unknowns) is
                        # captured to the raw log but not printed.

            except (ConnectionRefusedError, socket.timeout) as e:
                print(f"[probe] connect failed ({e}); retrying in {RECONNECT_DELAY_S}s. "
                      f"Is RL running with the Stats API enabled?")
                time.sleep(RECONNECT_DELAY_S)
            except OSError as e:
                print(f"[probe] socket error: {e}; reconnecting in {RECONNECT_DELAY_S}s")
                time.sleep(RECONNECT_DELAY_S)
    except KeyboardInterrupt:
        print()
        print("[probe] stopped.")
        return 0
    finally:
        raw_f.close()
        csv_f.close()


if __name__ == "__main__":
    sys.exit(main())
