# Ulticast

A lean, GPU-first screen capture / recorder / streamer with a dark Direct2D UI.
Built from scratch to have the smallest possible per-frame cost on the capture path.

[![build](https://github.com/modnite/ulticast/actions/workflows/build.yml/badge.svg)](https://github.com/modnite/ulticast/actions/workflows/build.yml)
[![latest release](https://img.shields.io/github/v/release/modnite/ulticast)](https://github.com/modnite/ulticast/releases/latest)

## Download & install

- **Installer (recommended):** grab `Ulticast-Setup-x.y.z.exe` from the
  [latest release](https://github.com/modnite/ulticast/releases/latest). Installs both executables,
  the FFmpeg runtime and the browser-overlay runtime (CEF); Start Menu and desktop shortcuts are
  optional tasks in the installer.
  Windows 10 2004+ or Windows 11, x64, NVIDIA GPU (NVENC).
- **Portable:** `Ulticast-portable-x.y.z.zip` — unzip anywhere and run `ulticast-ui.exe`.

Nothing driver-related is bundled: encoding uses the NVENC runtime that ships with your NVIDIA driver.

### Windows SmartScreen ("Windows protected your PC")

The binaries aren't Authenticode-signed yet, so Windows marks the download as *from the internet*
and SmartScreen shows its usual prompt. Two ways to handle it:

- **Right now, any build:** remove the download mark — right-click the file → **Properties** →
  tick **Unblock** → **OK**, or in PowerShell: `Unblock-File .\Ulticast-Setup-x.y.z.exe`.
  The prompt disappears because the file is no longer treated as an internet download.
- **Proper fix, one secret away:** the pipeline is already wired for signing. Add two repository
  secrets and every release is signed automatically:
  - `SIGNING_PFX_BASE64` — `[Convert]::ToBase64String([IO.File]::ReadAllBytes('cert.pfx'))`
  - `SIGNING_PFX_PASSWORD` — the `.pfx` password

  The workflow then signs `ulticast.exe` and `ulticast-ui.exe` before packing, and Inno Setup signs
  the installer **and** the uninstaller stub with the same certificate. Locally:
  `tools\sign.ps1 -PfxPath cert.pfx -PfxPassword ***`.

  Certificate options: **EV** code-signing certs earn SmartScreen trust immediately (~$400/yr);
  **OV** certs build reputation as downloads accumulate (~$200/yr); **Azure Trusted Signing** is
  Microsoft's cloud service (~$10/month, requires identity validation). All three work with the
  secrets above — no code changes needed.

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

![Ulticast UI — record](test/ui_final_idle.png)

![Ulticast UI — stream](test/ui_final_stream.png)

![Ulticast UI — settings](test/ui_final_setup.png)

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
- **Stage 6** multi-scene / multi-source engine (live scene switching) + two-page UI redesign — done, verified
- **Stage 7** usability pass: DPI-proof window (resizable/maximizable), idle preview, GPU picker +
  NVIDIA auto-select, CBR + full CQ range, encoder options, audio device/gain controls, live webcam
  drag, close-to-tray, start-menu shortcut option — done, verified
- **Stage 8** installer/UX hardening: installer force-closes any running instance (incl. tray-hidden
  UI and CEF helpers), single-instance app guard, carded Setup layout with proper control spacing — done, verified
- **Stage 9** Record / Stream / Settings surfaces with per-mode settings, independent runtime outputs
  (start recording and go live separately, no interruption), expanded source list (display / window /
  game / video devices incl. capture cards / mic / desktop audio / browser), per-scene sources with
  live add/configure/remove — done, verified
- **Stage 10** roadmap: mic noise gate (live threshold, CLI `--gate`) — done, verified
- **Stage 11** (unreleased): mic compressor (-1 dB safety clip), mic monitoring, multiple capture
  sources per scene (second monitor/window as a PiP layer), DirectShow camera fallback, audio device
  auto-recovery, themes (8 accents + light/dark), dead-space-free layout, cleaner action buttons with
  record/play/stop glyphs — built and tested locally (12/12 smoke tests)
- **Stage 12** Twitch OAuth device-code login (connect once, stream key stays current),
  DirectShow camera fallback, mic monitoring, audio device auto-recovery — released in v0.9.0
- Next: on-hardware validation on the RTX 3080 rig

## The UI (Ulticast-ui.exe)

Dark theme, custom-drawn controls, and **three surfaces**: **Record**, **Stream** and **Settings** —
each mode has its own layout, its own big action button and its own settings, so nothing is crammed
together (the same separation ShadowPlay/OBS users expect).

| Surface | Contents |
|---|---|
| **Record** | scenes + sources rail · live preview (stats overlaid) · **RECORDING** (path, container codec), **CLIPS** (replay toggle, buffer length, F9), **ENCODING** (CQ or CBR, preset, B-frames) · transport: **Start/Stop Recording**, Pause, Snap Clip, Open Folder |
| **Stream** | scenes + sources rail · live preview · **PLATFORM** (Twitch / YouTube / Facebook / custom, stream key or RTMP URL, reconnect attempts) · **STREAM QUALITY** (bitrate, H.264/HEVC, preset) · transport: **Go Live / End Stream**, Pause, Snap Clip |
| **Settings** | capture defaults (monitor, window, backend, fps, **GPU**), camera & audio defaults (devices, mic/desktop gain, chroma key), **audio filters** (noise gate, compressor with -1 dB safety clip), **appearance** (8 accents + light/dark), browser overlay URL, advanced encoder (lookahead, two-pass), behavior (idle preview, close to tray), troubleshooting, about |

**Sources** (per scene, OBS-style — add with **+ Add**, click a row to configure it):

| Type | Tag | Options |
|---|---|---|
| Display Capture | DISP | monitor — **add more than one to layer a second monitor as a PiP** |
| Window Capture | WIN | window picker (scrollable list) — also works as a PiP layer |
| Game Capture | GAME | monitor + DXGI duplication (fullscreen games, first capture) |
| Video Capture Device | VID | **any camera or capture card** — Media Foundation first, with an automatic **DirectShow fallback** for virtual cameras MF cannot see (some NVIDIA Broadcast builds, Snap Camera); multiple devices can run at once, each with its own PiP corner or free position (drag it on the preview) |
| Microphone | MIC | capture device |
| Desktop Audio | SYS | output device (loopback) |
| Browser Overlay | WEB | HTML file / https:// URL (set in Settings) |

- **Independent outputs**: Start Recording and Go Live are separate actions; either can start the
  session and the other can be switched on later without interrupting the first (recording and
  streaming use separate NVENC sessions).
- **Audio filters**: noise gate (2 ms attack / 120 ms hold / 150 ms release), a compressor with a
  -1 dB safety clip, and **mic monitoring** (hear the post-filter mic in your headphones); all
  adjustable live while recording (`--gate DB`, `--comp DB`, `--comp-ratio R`, `--monitor-mic`).
- **Device auto-recovery**: unplugging/replugging an output (or waking a sleeping device) restarts
  that endpoint in place — the session keeps running instead of losing audio.
- **Themes**: eight accent colours (violet, cyan, emerald, amber, rose, azure, magenta, slate) with
  light and dark variants; everything (buttons, switches, sliders, caption bar) follows the accent.
- **Layout**: no dead space — the rail's leftover room holds the activity log, mode cards fill their
  rows, and the Settings columns are balanced top to bottom.
- **Scenes**: each scene owns its source list; switching applies live while recording.
- **Idle preview**: while not producing output, Ulticast composites the scene into the preview only —
  the camera opens only when recording, streaming or the replay buffer is armed.
- **Replay buffer**: toggle it in Record > CLIPS; Snap Clip (F9) saves the last moments.
- **No implicit hooks**: WGC is the default; DXGI duplication is an explicit per-source choice.
- **Dropped-frame + uptime indicators**, live activity strip, tray menu with the same actions.
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
| Output | `--out PATH\|none`, `--seconds N`, `--codec hevc\|h264`, `--cqp N` (1–51), `--cbr --bitrate N`, `--preset N`, `--bframes 0-4`, `--lookahead` |
| Scene | `--webcam`, `--webcam-device`, `--webcam-pos`, `--webcam-size`, `--chroma RRGGBB`, `--chroma-layer webcam\|capture`, `--browser URL\|FILE`, `--browser-size WxH` |
| Clips | `--replay`, `--replay-seconds`, `--replay-prefix`, `--hotkey`, `--clip-at` |
| Stream | `--platform`, `--key`, `--rtmp URL`, `--also-record PATH`, `--reconnect N` |
| Audio | `--no-audio`, `--no-desktop-audio`, `--mic`, `--mic-device`, `--desktop-device`, `--mic-gain X`, `--desktop-gain X`, `--gate DB`, `--comp DB`, `--comp-ratio R`, `--monitor-mic`, `--audio-bitrate N`, `--tracks 1\|2` |
| Diagnostics | `--diagnostics`, `--open-logs`, `--version`, `--twitch-login [--twitch-client-id ID]` |

## Streaming targets

| Platform | Ingest |
|---|---|
| Twitch | `rtmp://live.twitch.tv/app` |
| YouTube | `rtmps://a.rtmps.youtube.com/live2` |
| Facebook | `rtmps://live-api-s.facebook.com:443/rtmp/` |
| Kick / other | custom RTMP/RTMPS URL |

### Twitch account (no stream key pasting)

Ulticast ships a registered public Twitch client and uses the **OAuth device-code flow** — no
secret, no redirect, nothing to configure:

- In the UI: **Stream → Twitch Account → Connect Twitch**. A code appears and the activation page
  opens in your browser; enter the code and the app fetches your channel name and stream key
  automatically (the key is stored DPAPI-encrypted like any pasted key). Afterwards the button
  becomes **Refresh stream key**, which silently refreshes the token and re-reads the key.
- From a terminal: `ulticast.exe --twitch-login` prints the code, waits for confirmation, then
  prints the channel and stream key.

Scopes requested: `channel:read:stream_key` only.

## Known limitations

1. Window capture returns black in the test VM (WGC); monitor capture and the compositor are fine.
   DXGI duplication on this VM delivers black until the desktop first updates, then real content.
3. Frame-counted CFR: capture slightly above 60 fps can stretch video media ~1% vs wall time.
4. Pause applies to recordings; the final AAC block (~21 ms) is not flushed at stop.
5. Browser overlays spin up a full Chromium (helper processes, ~300 MB runtime); the first
   paint takes a second or two after a session starts.
6. Cameras are exclusive: while recording, the selected camera belongs to Ulticast. The idle
   preview never opens the camera, and Stop releases it immediately.
7. Virtual cameras that only register with DirectShow (some NVIDIA Broadcast builds) may not show
   up in the webcam list; check with the Windows Camera app — if it appears there, Ulticast sees it.

## Roadmap

- Everything on the original roadmap is done; next is on-hardware validation on the RTX 3080 rig
  and whatever that turns up.

### Done

- **Twitch login (OAuth device-code flow)**: Connect Twitch in the UI (or `--twitch-login` on the
  CLI) links the account and pulls the stream key automatically; tokens are refreshed silently and
  stored DPAPI-encrypted.
- **Audio filters, part 1**: mic noise gate with live threshold (`--gate DB`).
- **Audio filters, part 2**: mic compressor with live threshold/ratio and a -1 dB safety clip
  (`--comp DB`, `--comp-ratio R`).
- **Audio filters, part 3**: mic monitoring to the default output (`--monitor-mic`), with a
  headphones hint.
- **Multiple capture sources per scene**: extra display/window captures composite as PiP layers
  (position configurable per source).
- **DirectShow camera fallback**: video devices are listed from both Media Foundation and
  DirectShow, and the engine falls back to a DirectShow graph (SampleGrabber) when MF cannot open
  a device — verified with a real camera (`--force-dshow`).
- **Audio device auto-recovery**: lost endpoints reconnect in place instead of ending the session.
- **Themes**: 8 accent colours, light/dark, caption bar follows the accent.
