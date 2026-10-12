# Third-party notices

Ulticast's installer and portable build bundle the following components. Their
licenses are included in the release archives; the texts also ship next to the
binaries where the upstream projects provide them.

| Component | Version | License | Used for |
|---|---|---|---|
| FFmpeg (BtbN LGPL shared build) | master, LGPL configuration | LGPL-3.0-or-later (see `ffmpeg/LICENSE.txt`) | Muxing (MP4/MKV), RTMP(S) transport, AAC encoding |
| Chromium Embedded Framework (CEF) | 150.0.22 (Chromium 150) | BSD-3-Clause (see `cef/LICENSE.txt` in the CEF distribution) | Browser overlay rendering (alerts, chat widgets, HTML) |
| nv-codec-headers | n13.1.15.0 | MIT | NVENC API headers (compile-time only) |
| NVIDIA NVENC runtime | installed with the GPU driver | NVIDIA driver license | Hardware video encoding (H.264/HEVC) |

Notes:

- FFmpeg is linked dynamically and used only as an unmodified library; Ulticast
  does not statically link FFmpeg.
- CEF runs in its own helper processes and renders offscreen into the compositor.
- The NVENC API is loaded at runtime from the installed NVIDIA driver
  (`nvEncodeAPI64.dll`); no NVIDIA binaries are redistributed here.
