# SyncComms

**Hear what happened.** A BakkesMod plugin that captures voice comms and game audio during Rocket League matches, then plays them back perfectly synced to your replays.

---

## What It Does

SyncComms records audio from Discord, your mic, or any application while you play. When you open a saved replay, the captured audio plays back locked to the replay timeline — scrub, pause, rewind, and the audio follows.

Audio is automatically segmented around goals, compressed to OGG, and stored alongside your replays as lightweight sidecar files.

## Features

- **Per-app audio capture** — record Discord, game audio, or any running process individually via WASAPI loopback
- **Microphone mixing** — optionally capture your own mic input alongside app audio
- **Goal-based segmentation** — audio automatically splits at each goal for clean, event-driven playback
- **Time-locked replay sync** — audio position tracks replay elapsed time, including scrub and pause
- **OGG compression** — captured WAV files are compressed to OGG Vorbis for minimal disk usage
- **In-game UI** — ImGui overlay for recording status, playback controls, volume, and latency adjustment
- **Configurable** — sample rate (8–96 kHz), mono/stereo, latency offset (±500ms), target process selection

## Who It's For

**Coaches** — Review match replays with full team comms to analyze communication, callouts, and decision-making in context.

**Players** — Rewatch your own games with voice chat intact. Hear what was said during that whiff or that clutch save.

**Content Creators** — Capture synced comms for replay-based content without juggling separate audio recordings or manual alignment.

## Requirements

- Windows 10 (version 2004 or later)
- [Rocket League](https://www.rocketleague.com/) (Epic Games)
- [BakkesMod](https://bakkesmod.com/)

## Installation

1. Download the latest `SyncComms.zip` from [Releases](../../releases)
2. Extract and copy the `bakkesmod` folder into `%APPDATA%/bakkesmod/` (merge when prompted)
3. Open `%APPDATA%/bakkesmod/bakkesmod/cfg/plugins.cfg` and add:
   ```
   plugin load synccomms
   ```
4. Launch Rocket League — the plugin loads automatically

### Usage

- Open BakkesMod settings (`F2`) > Plugins > SyncComms
- Select which application to capture (Discord, etc.)
- Play a match — audio captures automatically
- Open a saved replay to hear the synced audio

## Building from Source

```bash
git clone --recurse-submodules https://github.com/<you>/CommSyncRL.git
cd CommSyncRL

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The built plugin lands at `build/plugins/Release/SyncComms.dll`.

To package a distributable zip:

```bash
./export.sh
# Output: dist/SyncComms.zip
```

### Dependencies (included as submodules)

| Library | Purpose |
|---------|---------|
| [miniaudio](https://github.com/mackron/miniaudio) | Audio device I/O and decoding |
| [libvorbis](https://github.com/xiph/vorbis) / [libogg](https://github.com/xiph/ogg) | OGG Vorbis encoding |
| [nlohmann/json](https://github.com/nlohmann/json) | Sidecar JSON serialization |
| [Dear ImGui](https://github.com/ocornut/imgui) | In-game UI overlay |
| [BakkesMod SDK](https://github.com/bakkesmodorg/BakkesModSDK) | Plugin API |

## License

This project is not open for contributions. Feel free to fork for personal use.
