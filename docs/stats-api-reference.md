# Rocket League Stats API — internal reference

Source: <https://www.rocketleague.com/en/developer/stats-api> (Season 22 / 2026).
The HTML of the page is checked in at the repo root (`Rocket League Stats API.html`)
in case the URL goes 403 again. Plain-text dump in `tools/stats_api_text.txt`.

This file is the source of truth we work from. **Do not guess the wire shape
from intuition** — go to the official table or the example payload first.

---

## 1. Configuration

Edit `<RL Install>\TAGame\Config\DefaultStatsAPI.ini` *before* launching RL.
Settings are read once at process start.

| Setting          | Type   | Default       | Notes                                             |
|------------------|--------|---------------|---------------------------------------------------|
| `PacketSendRate` | float  | `0` (disabled)| Packets/sec for `UpdateState`. **Must be > 0** to enable the socket. Capped at 120. |
| `Port`           | int    | `49123`       | Local port the socket listens on.                 |

Example:
```ini
PacketSendRate=30
Port=49123
```

## 2. Transport

- **Local TCP socket** on `127.0.0.1:<Port>`. Doc calls it a "web socket" but
  in practice every consumer (sos-rl-converter, our probe) treats it as raw
  TCP/JSON. No handshake, no auth.
- Each message is a JSON object. **No length prefix or framing** — consumers
  must do streaming JSON parse (we use a brace-depth scanner in
  `StatsApiClient.cpp::DrainJsonObjects`).
- Newlines may or may not appear between objects; do not rely on them.

## 3. Envelope shape

```json
{
  "Event": "EventName",
  "Data": { /* event-specific payload */ }
}
```

- Field names use **TitleCase**: `Event`, `Data`, `MatchGuid`, `Players`, etc.
- `Data` is **a JSON object**, not a JSON-encoded string. Our normalizer in
  `main.cpp` defensively handles the string case (some forks/emitters were
  reported to do that), but the official format is an object.
- Some `Data` fields are **CONDITIONAL** (only present when relevant) or
  **SPECTATOR** (only present if the client is spectating or on the player's
  team). Treat any field you read as potentially missing.

## 4. UpdateState (the tick)

`UpdateState` is sent at `PacketSendRate` Hz while a match is in progress.
Event-driven payloads (goals, etc.) are emitted **on the same tick** the
event occurs regardless of rate.

### 4.1 Live-match shape (what we receive when bReplay=false)

```json
{
  "Event": "UpdateState",
  "Data": {
    "MatchGuid": "...",
    "Players": [{
      "Name": "PlayerA", "PrimaryId": "Steam|123|0",
      "Shortcut": 1, "TeamNum": 0,
      "Score": 125, "Goals": 1, "Shots": 2, "Assists": 0,
      "Saves": 1, "Touches": 14, "CarTouches": 3, "Demos": 0,
      "bHasCar": true, "Speed": 1200, "Boost": 45,
      "bBoosting": true, "bOnGround": true, "bOnWall": false,
      "bPowersliding": false, "bDemolished": false, "bSupersonic": true
      // Attacker present only when bDemolished=true
    }],
    "Game": {
      "Teams": [{ "Name": "Blue", "TeamNum": 0, "Score": 1,
                  "ColorPrimary": "0000FF", "ColorSecondary": "0000AA" }],
      "TimeSeconds": 180,
      "bOvertime": false,
      "Ball": { "Speed": 850.5, "TeamNum": 0 },
      "bReplay": false,
      "bHasWinner": false, "Winner": "",
      "Arena": "Stadium_P",
      "bHasTarget": true,
      "Target": { "Name": "PlayerA", "Shortcut": 1, "TeamNum": 0 }
      // ⚠ NO Frame, NO Elapsed in live matches
    }
  }
}
```

### 4.2 Replay-viewing shape (bReplay=true)

Same shape as live, **plus** two extra fields inside `Game`:

| Field    | Type  | Description                                                  |
|----------|-------|--------------------------------------------------------------|
| `Frame`  | int   | CONDITIONAL. Current frame number if a replay is active.     |
| `Elapsed`| float | CONDITIONAL. Seconds elapsed since game start (replay only). |

These are also conditional during in-match goal cinematics
(`bReplay` is `true` for those too).

### 4.3 Per-player fields (full reference)

| Field           | Type   | Visibility   | Notes                                              |
|-----------------|--------|--------------|----------------------------------------------------|
| `Name`          | string | always       | Display name.                                      |
| `PrimaryId`     | string | always       | `Platform\|Uid\|Splitscreen` (e.g. `Steam\|123\|0`). |
| `Shortcut`      | int    | always       | Spectator shortcut number.                         |
| `TeamNum`       | int    | always       | 0 = Blue, 1 = Orange.                              |
| `Score`/`Goals`/`Shots`/`Assists`/`Saves`/`Touches`/`CarTouches`/`Demos` | int | always | Match-cumulative stats. |
| `bHasCar`       | bool   | SPECTATOR    | True if the player has a vehicle.                  |
| `Speed`         | float  | SPECTATOR    | Unreal Units/second.                               |
| `Boost`         | int    | SPECTATOR    | 0–100.                                             |
| `bBoosting`/`bOnGround`/`bOnWall`/`bPowersliding`/`bDemolished`/`bSupersonic` | bool | SPECTATOR | Self-explanatory. |
| `Attacker`      | object | CONDITIONAL  | Present only when `bDemolished` is true. `{Name, Shortcut, TeamNum}`. |

### 4.4 Per-game fields inside `Game`

| Field            | Type    | Notes                                                       |
|------------------|---------|-------------------------------------------------------------|
| `Teams[]`        | array   | One entry per team, ordered by `TeamNum`.                  |
| `TimeSeconds`    | int     | Seconds **remaining** in the match (counts DOWN to 0).      |
| `bOvertime`      | bool    | True during OT.                                             |
| `Ball`           | object  | `{ Speed: float, TeamNum: int }`. `TeamNum=255` if untouched. |
| `bReplay`        | bool    | True during goal replay OR history replay.                  |
| `bHasWinner`     | bool    | True if a team has won.                                     |
| `Winner`         | string  | Winning team name. `""` if no winner yet.                   |
| `Arena`          | string  | Map asset name (e.g. `"Stadium_P"`).                        |
| `bHasTarget`     | bool    | True if the client is viewing a specific vehicle.           |
| `Target`         | object  | CONDITIONAL. `{Name, Shortcut, TeamNum}`. Empty/0 fields if no target. |
| `Frame`          | int     | CONDITIONAL — replay only.                                  |
| `Elapsed`        | float   | CONDITIONAL — replay only. "Seconds elapsed since game start." |

## 5. Events (alphabetical)

Every event below has a top-level `Data.MatchGuid` (string, "Only set for online or LAN matches").

### `BallHit`
> Sent one frame after the ball is hit.

```json
"Data": {
  "MatchGuid": "...",
  "Players": [{ "Name": "...", "Shortcut": 1, "TeamNum": 0 }],
  "Ball": {
    "PreHitSpeed": 0,
    "PostHitSpeed": 1450.2,
    "Location": { "X": -512, "Y": 100, "Z": 200 }
  }
}
```

### `ClockUpdatedSeconds`
> Sent when the in-game clock has changed.

```json
"Data": { "MatchGuid": "...", "TimeSeconds": 180, "bOvertime": false }
```

### `CountdownBegin`
> Sent at the start of each round when the countdown starts.

Fires both at first kickoff and after every goal. Empty payload other than `MatchGuid`.

### `CrossbarHit`
> Sent when the ball hits a crossbar.

```json
"Data": {
  "MatchGuid": "...",
  "BallLocation": { "X": 120, "Y": -2944, "Z": 320 },
  "BallSpeed": 870.3,
  "ImpactForce": 127.5,
  "BallLastTouch": {
    "Player": { "Name": "...", "Shortcut": 1, "TeamNum": 0 },
    "Speed": 120
  }
}
```

### `GoalReplayStart` / `GoalReplayEnd` / `GoalReplayWillEnd`

In-match goal cinematic events. Empty payload other than `MatchGuid`.
- `GoalReplayStart` — cinematic begins.
- `GoalReplayWillEnd` — fired when the ball "explodes" near the end of the cinematic. **Not fired if the user skips the cinematic.**
- `GoalReplayEnd` — cinematic ends (always fires, including when skipped).

> ⚠ **EMPIRICAL (2026-04-30, Season 22): these three events are NOT emitted.**
> Across three recorded sessions (`tools/stats_api_raw_*.jsonl`) containing
> multiple live goals with visible cinematics (and one full `MatchEnded`
> match), zero `GoalReplay*` events appeared on the wire. Detect the
> cinematic from `UpdateState` instead: it is the period where
> `Game.bReplay == true` with **no** `Frame` field. The 0→1 flip lands
> ~3.5s after the live `GoalScored`; the cinematic lasts ~2–3.5s when
> skipped. CaptureOrchestrator closes segments on this flip.

### `GoalScored`
> Sent when a goal is scored.

This is the canonical goal event — richer than `StatfeedEvent[Goal]`.

> ⚠ **EMPIRICAL (2026-04-30): `GoalScored` fires TWICE per goal.** The real
> one arrives on the goal tick (while `bReplay=false`) with `GoalTime` and
> `Scorer` populated; a second one re-fires during the in-match cinematic
> (~5–7s later, while raw `bReplay=true`) with `GoalTime=0` and an empty
> `Scorer`. Gate on the last raw `bReplay` being `false` to accept only the
> real one (CaptureOrchestrator does this for the goal-anchor stamp).

```json
"Data": {
  "MatchGuid": "...",
  "GoalSpeed": 87.3,
  "GoalTime": 127.5,
  "ImpactLocation": { "X": 0, "Y": -2944, "Z": 320 },
  "Scorer":   { "Name": "PlayerA", "Shortcut": 1, "TeamNum": 0 },
  "Assister": { "Name": "PlayerC", "Shortcut": 3, "TeamNum": 0 },
  "BallLastTouch": {
    "Player": { "Name": "PlayerA", "Shortcut": 1, "TeamNum": 0 },
    "Speed": 125
  }
}
```

| Field              | Type   | Notes                                                  |
|--------------------|--------|--------------------------------------------------------|
| `GoalSpeed`        | float  | Ball speed when it crossed the goal line (UU/sec).     |
| `GoalTime`         | float  | **Length of the previous round in seconds.** Useful for sanity-checking segment durations. |
| `ImpactLocation`   | vector | World position at score moment.                        |
| `Scorer`           | object | `{Name, Shortcut, TeamNum}`.                           |
| `Assister`         | object | CONDITIONAL — same shape as Scorer, only on assists.   |
| `BallLastTouch`    | object | `{Player, Speed}`.                                     |

### `MatchCreated`
> Sent when all teams are created and replicated. Empty payload other than `MatchGuid`.

### `MatchInitialized`
> Sent when the **first** countdown starts. Empty payload other than `MatchGuid`.

### `MatchDestroyed`
> Sent when leaving the game. Empty payload other than `MatchGuid`.

### `MatchEnded`
> Sent when the match ends and a winner is chosen.

```json
"Data": { "MatchGuid": "...", "WinnerTeamNum": 0 }
```

### `MatchPaused` / `MatchUnpaused`
> Sent when the game is paused/unpaused **by a match admin**. Empty payload.
Note: this is admin pause, not host pause for casuals.

### `PodiumStart`
> Sent when the game enters the podium state after the match ends. Empty payload.

### `ReplayCreated`
> Sent when a replay is initialized. **Only for replays loaded via the Match
> History menu** — does NOT pertain to in-match goal cinematics.

Empty payload other than `MatchGuid`. This is the right hook for "user opened a saved replay."

### `RoundStarted`
> Sent when the game enters the active state (after the countdown finishes).

Empty payload other than `MatchGuid`. This is "the match is now live and playable."

### `StatfeedEvent`
> Sent when someone earns a stat.

```json
"Data": {
  "MatchGuid": "...",
  "EventName": "Demolish",
  "Type": "Demolition",
  "MainTarget":      { "Name": "...", "Shortcut": 1, "TeamNum": 0 },
  "SecondaryTarget": { "Name": "...", "Shortcut": 2, "TeamNum": 1 }
}
```

| Field             | Type   | Notes                                                          |
|-------------------|--------|----------------------------------------------------------------|
| `EventName`       | string | Asset name (e.g. `Demolish`, `Save`, `Goal`, `Shot`, `Win`, `EpicSave`). |
| `Type`            | string | Localized display label (e.g. `Demolition`).                  |
| `MainTarget`      | object | Player who earned the stat.                                    |
| `SecondaryTarget` | object | CONDITIONAL — second player involved (e.g. demolished player). |

For goals specifically, prefer `GoalScored` over `StatfeedEvent[Goal]` — richer fields.

---

## 6. Practical mappings (what we should be doing)

### Capture-time time base (live match)

There is **no `Frame` or `Elapsed` field during live capture** — the doc is
explicit: those fields are conditional on `bReplay=true`. So we can't stamp
segments with the same value the replay viewer will read.

What we *do* have during live:
- `Game.TimeSeconds` (countdown clock — not monotonic; pauses, resets in OT).
- `ClockUpdatedSeconds` events for each second tick.
- Wall-clock arrival time of each tick.
- `GoalScored.GoalTime` ("Length of the previous round in seconds") — useful retroactive measurement.

Current strategy: **wall-clock-since-first-UpdateState** as the
seconds-since-match-start estimate. Compare against the replay viewer's
`Elapsed` (which the doc says is also "seconds elapsed since game start").
If observed offset between them is constant per match, we can calibrate.

### Playback-time time base (replay viewing)

The replay viewer provides both `Frame` and `Elapsed`. Per the doc, `Elapsed`
*is* the canonical "seconds since game start" — same definition our capture
wall-clock approximates. **Prefer `Elapsed` for sync**, fall back to
`Frame * (1/30)` if `Elapsed` is missing/stuck.

(The user observed `Elapsed=9.1, Frame=1` at the start of a replay viewer
session — likely RL adds a few seconds of leader/countdown to the recording
before frame 1. Either accept this constant offset, or use Frame as the
fallback.)

### Cleaner event mapping for the orchestrator

| Today's logic                              | Cleaner with full doc                   |
|--------------------------------------------|-----------------------------------------|
| Goal → `StatfeedEvent` with `Type=Goal`    | Goal → `GoalScored` (always fires; ignore the cinematic re-fire, see §5) |
| Goal cinematic detection via `bReplay`     | ~~`GoalReplayStart`/`GoalReplayEnd`~~ NOT EMITTED (see §5) — use the raw `bReplay` 0→1 flip with `Frame` absent |
| Kickoff → `CountdownBegin` + first UpdateState bReplay check | Kickoff → `RoundStarted` (post-countdown, definitively live) |
| First-of-match arming → `MatchCreated`     | Same (or `MatchInitialized`)            |
| End → `MatchEnded`                         | Same                                    |
| Replay-open detection                      | `ReplayCreated` is the explicit hook (confirmed on the wire) |

### Good defaults

- `PacketSendRate=30` is sufficient for our needs (audio sync precision is way
  coarser than the tick rate). Higher rates just give chattier `BallHit`/etc.
- `bReplay` is `true` during goal cinematics. Any "is this user watching a
  saved replay?" detection should be gated on `ReplayCreated` having fired —
  not just `bReplay=true`.

## 7. What's NOT in the API

These are useful but absent — we can't get them from the Stats API:
- Match playlist / queue type (Casual 3v3, Ranked Doubles, etc.).
- Player ranks / MMR.
- Replay file path on disk.
- Any access to past matches / match history.
- Inputs (button presses, controller state).
- Voice / quick chat / text chat.
- Per-player car/cosmetic info.

For replay-file-on-disk matching we can parse the `.replay` header
ourselves (rrrocket / RocketLeagueReplayParser) since the API doesn't tell
us which file got written.
