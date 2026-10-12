# Ulticast

A lean, GPU-first screen capture / recorder / streamer with a dark Direct2D UI.
Built from scratch to have the smallest possible per-frame cost on the capture path.

[![build](https://github.com/modnite/ulticast/actions/workflows/build.yml/badge.svg)](https://github.com/modnite/ulticast/actions/workflows/build.yml)
[![latest release](https://img.shields.io/github/v/release/modnite/ulticast)](https://github.com/modnite/ulticast/releases/latest)

## Download & install

- **Installer (recommended):** grab `Ulticast-Setup-x.y.z.exe` from the
  [latest release](https://github.com/modnite/ulticast/releases/latest). Installs both executables,
  the FFmpeg runtime, the browser-overlay runtime (CEF), and Start Menu / desktop shortcuts.
  Windows 10 2004+ or Windows 11, x64, NVIDIA GPU (NVENC).
- **Portable:** `Ulticast-portable-x.y.z.zip` — unzip anywhere and run `ulticast-ui.exe`.

Nothing driver-related is bundled: encoding uses the NVENC runtime that ships with your NVIDIA driver.

## Bug reports & diagnostics

Everything needed for a good report is built in:

- In the UI: **Report Bug** collects a redacted system report
  (`%LOCALAPPDATA%\Ulticast\diagnostics.txt`), opens it, and takes you to the issue form.
  **Logs** opens the log folder.
- From a terminal: `ulticast.exe --diagnostics` (prints and saves the report), `--open-logs`.
- Every run writes a log to `%LOCALAPPDATA%\Ulticast\logs` (newest 20 kept) with per-second audio
  device rates, capture-backend decisions, encoder/replay state and errors.
- The report covers OS/CPU/RAM/GPU, monitors, audio devices, cameras, the NVENC API version and the
  log tail. **The stream key is never included** (settings are redacted).

Please attach `diagnostics-*.txt` and the newest log to
[the bug report form](https://github.com/modnite/ulticast/issues/new?template=bug_report.yml).

![Ulticast UI](test/ui_final_idle.png)

## Architecture

```
                     ┌──────────────────────────── Session (engine) ───────────────────────────┐
 capture (WGC/DXGI) ─┤ scene texture ─ compositor (webcam PiP) ─┬─ rec encoder (HEVC/H.264) ─ mux ─ file
                     │                                        ├─ stream encoder (H.264 CBR) ─ mux ─ RTMP(S)
                     │                                        └─ replay encoder ─ RAM packet ring ─ clip
 audio (WASAPI loopback+mic) ─ mix ─ AAC ───────────────────────┴─ all outputs
                     └─────────────────────────────────────────────────────────────────────────┘
   driven by:  Ulticast.exe (CLI, automation)   or   Ulticast-ui.exe (the UI)
```

- **One D3D11 device, one capture thread** for the video path; the scene is composited in a single
  pass directly into the NVENC input texture.
- **FFmpeg does container/network only** (plus AAC).
- **Replay buffer holds encoded packets in RAM** (30 s of 1080p60 is tens of MB, not 14 GB).
- The UI owns its own D3D device; the live preview travels over a shared texture, so the capture
  thread and the UI never share a command context.

## Status

- **Stage 1** capture / NVENC / muxing — done, verified
- **Stage 2** audio, pause, streaming + reconnect — done, verified
- **Stage 3** DXGI backend, scene compositor, webcam overlay, replay buffer, hotkeys — done, verified
- **Stage 4** UI (Direct2D, tray, settings, dual output) — done, verified, polished
- **Stage 5** chroma key + **browser overlays (CEF offscreen rendering)** + **DXGI cursor compositing** — done, verified
- Next: on-hardware validation on the RTX 3080 rig

## The UI (Ulticast-ui.exe)

Dark theme, custom-drawn controls, live scene preview, and everything the engine can do:

| Area | Controls |
|---|---|
| Source | monitor picker, window picker (refreshed on open), capture backend (auto / WGC / DXGI), fps |
| Camera & audio | webcam toggle + device, desktop loopback toggle, mic toggle + device, **chroma key toggle + color + PiP corner** |
| Video | HEVC / H.264, NVENC preset P1–P7, CQ slider 16–30 |
| Output | **Record to file** toggle + path + Browse, **Go Live** toggle + platform + stream key |
| Clips | replay toggle + buffer length, Snap Clip (F9 global hotkey) |
| Overlay | browser overlay toggle + HTML file / https:// URL (alerts, chat widgets, custom HTML) |
| Transport | Start/Stop, Pause/Resume, Snap Clip, Open Folder |
| Activity strip | live engine messages (device init, file path, warnings, errors) |
| Preview | live scene with resolution/fps overlay and pulsing REC dot |

- **Dual output**: record to disk *and* stream at the same time (separate NVENC sessions).
- **Live stats**: elapsed time, frames, fps, throughput, active devices, backend, resolution.
- **Tray icon**: closing the window hides it; the tray menu can Start/Stop/Pause/Snap/Exit.
- **Settings** persist to `%APPDATA%\Ulticast\settings.ini`; the stream key is stored
  **DPAPI-encrypted** (per user, never plaintext).

## Verified on the RTX 2060 test machine

| Test | Result |
|---|---|
| UI launch + rendering | dark D2D surface, custom widgets, live preview |
| UI click-driven Start/Stop | recorded `Videos\Ulticast\Recording.mkv` (HEVC + AAC) |
| Live preview | scene texture shared to the UI, ~20 fps refresh, resolution/fps overlay |
| Activity strip | engine log lines surfaced in the UI ("Recording started", file path) |
| **Chroma key** | green window keyed to pure black at its pixels; control run unkeyed; unrelated pixels untouched |
| **Browser overlay (CEF)** | HTML rendered offscreen by Chromium 150 and composited: red square, green text, and a 50% white band blended mathematically exactly over the desktop; transparent areas show through |
| **DXGI cursor** | cursor shape queried via GDI (works with a parked cursor, where duplication reports nothing); tip/body pixels verified at a known coordinate while everything around stays untouched |
| Tray + hotkey | tray icon registered; F9 clip hotkey registered globally |
| Settings persistence | `settings.ini` written and re-loaded on next launch |
| Engine regressions (CLI) | pause (8 s → 6 s media), webcam + replay + audio combo, RTMP round-trip |
| **Dual output** | H.264 → RTMP and HEVC → disk simultaneously, both ~5.95 s |
| Footprint | 34% of one core / 116 MB with webcam + replay + recording + audio |

## Build from source

Requirements: Visual Studio 2022 (C++ tools), CMake 3.28+, Windows SDK, git, and ~4 GB of disk for
the vendored dependencies.

```
git clone https://github.com/modnite/ulticast.git
cd ulticast
powershell -ExecutionPolicy Bypass -File tools\fetch-deps.ps1
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

Produces `ulticast.exe` (CLI) and `ulticast-ui.exe` (GUI) in `build/Release`.
`tools/fetch-deps.ps1` downloads the pinned CEF 150 distribution, the BtbN FFmpeg LGPL shared build
and nv-codec-headers into `third_party/` (git-ignored).

CI (`.github/workflows/build.yml`) builds on a Windows runner for every push; pushing a `v*` tag
produces the installer and portable zip on the Releases page. `tools/smoke-test.ps1` is the local
end-to-end test (requires an NVIDIA GPU).

## CLI

```
Ulticast --list | --list-audio
Ulticast --monitor 0 --seconds 30 --out clip.mkv
Ulticast --monitor 0 --webcam --webcam-device ZV-1 --replay --hotkey F9
Ulticast --platform twitch --key <key> --also-record backup.mkv
```

| Area | Flags |
|---|---|
| Capture | `--monitor N`, `--window T`, `--backend auto\|wgc\|dxgi`, `--adapter N`, `--fps N` |
| Output | `--out PATH\|none`, `--seconds N`, `--codec hevc\|h264`, `--cqp N`, `--cbr --bitrate N`, `--preset N` |
| Scene | `--webcam`, `--webcam-device`, `--webcam-pos`, `--webcam-size`, `--chroma RRGGBB`, `--chroma-layer webcam\|capture`, `--browser URL\|FILE`, `--browser-size WxH` |
| Clips | `--replay`, `--replay-seconds`, `--replay-prefix`, `--hotkey`, `--clip-at` |
| Stream | `--platform`, `--key`, `--rtmp URL`, `--also-record PATH`, `--reconnect N` |
| Audio | `--no-audio`, `--no-desktop-audio`, `--mic`, `--mic-device`, `--desktop-device`, gains, `--tracks 1\|2` |
| Diagnostics | `--diagnostics`, `--open-logs`, `--version` |

## Streaming targets

| Platform | Ingest |
|---|---|
| Twitch | `rtmp://live.twitch.tv/app` |
| YouTube | `rtmps://a.rtmps.youtube.com/live2` |
| Facebook | `rtmps://live-api-s.facebook.com:443/rtmp/` |
| Kick / other | custom RTMP/RTMPS URL |

## Known limitations

1. Window capture returns black in the test VM (WGC); monitor capture and the compositor are fine.
   DXGI duplication on this VM delivers black until the desktop first updates, then real content.
3. Frame-counted CFR: capture slightly above 60 fps can stretch video media ~1% vs wall time.
4. Pause applies to recordings; the final AAC block (~21 ms) is not flushed at stop.
5. Browser overlays spin up a full Chromium (helper processes, ~300 MB runtime); the first
   paint takes a second or two after a session starts.
