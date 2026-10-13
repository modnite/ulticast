// Ulticast command-line interface.
//
// With no arguments this launches the UI (Ulticast-ui.exe); with arguments it
// drives a headless Session directly (used for automation and regression tests).

#include <windows.h>
#include <shellapi.h>
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/capture_wgc.hpp"
#include "core/browser_cef.hpp"
#include "core/d3d.hpp"
#include "core/diag.hpp"
#include "core/log.hpp"
#include "core/twitch_auth.hpp"
#include "core/version.hpp"
#include "core/wasapi_capture.hpp"
#include "core/webcam_mf.hpp"
#include "session.hpp"

extern "C" void av_log_set_level(int level);

static std::atomic<bool> g_stop{false};
static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

struct PlatformPreset {
    const char* name;
    const char* url;
    uint32_t bitrate;
};

static const PlatformPreset kPlatforms[] = {
    {"twitch", "rtmp://live.twitch.tv/app", 6000},
    {"youtube", "rtmps://a.rtmps.youtube.com/live2", 6000},
    {"facebook", "rtmps://live-api-s.facebook.com:443/rtmp/", 4000},
    {"custom", "", 6000},
};

struct Options {
    bool ui = false;
    bool list = false;
    bool listAudio = false;
    bool diagnostics = false;
    bool version = false;
    bool openLogs = false;
    bool twitchLogin = false;
    std::string twitchClientId;
    int monitor = 0;
    int adapter = -1;
    std::string backend = "auto";
    std::wstring window;
    std::string out;
    double seconds = 10.0;
    uint32_t fps = 60;
    bool hevc = true;
    uint32_t cqp = 20;
    bool cbr = false;
    uint32_t bitrate = 0;
    int preset = 5;
    int bframes = 0;
    bool lookahead = false;

    bool noAudio = false;
    bool desktopAudio = true;
    bool mic = false;
    std::wstring desktopDevice = L"default";
    std::wstring micDevice = L"default";
    float desktopGain = 1.0f;
    float micGain = 1.0f;
    bool gate = false;
    int gateDb = -45;
    bool comp = false;
    int compDb = -18;
    int compRatioX10 = 30;
    bool monitorMic = false;
    int audioBitrate = 160;
    int tracks = 1;

    std::string platform;
    std::string streamKey;
    std::string rtmpUrl;
    std::string alsoRecord;
    int reconnect = 5;

    double pauseAt = -1.0;
    double resumeAt = -1.0;

    bool webcam = false;
    std::wstring webcamDevice = L"default";
    std::string webcamPos = "bottomright";
    int webcamWidth = 0;

    bool replay = false;
    int replaySeconds = 30;
    std::string replayPrefix = "Replay";
    int hotkeyReplay = VK_F9;
    double clipAt = -1.0;

    bool chroma = false;
    std::string chromaColor = "00FF00";
    int chromaLayer = 0;  // 0 = webcam, 1 = capture

    std::string browserUrl;
    int browserWidth = 0, browserHeight = 0;
    bool showCursor = true;
    bool forceDshow = false;
};

static int ParseHotkey(const std::string& s) {
    std::string t = s;
    for (auto& c : t) c = (char)toupper((unsigned char)c);
    if (t.empty() || t == "NONE" || t == "0") return 0;
    if (t.size() >= 2 && t[0] == 'F') {
        int n = atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return -1;
}

static void PrintUsage() {
    std::printf(
        "  Ulticast -- lean GPU capture/record/stream\n\n"
        "No arguments launches the UI. Flags drive a headless session:\n\n"
        "  Ulticast --list | --list-audio\n"
        "  Ulticast --diagnostics          system/device report for bug reports\n"
        "  Ulticast --twitch-login         link a Twitch account (device code) and print the stream key\n"
        "  Ulticast --version\n"
        "  Ulticast [--monitor N | --window \"title\"] [options]\n"
        "  Ulticast --platform twitch --key <streamkey> [options]\n\n"
        "Capture:  --monitor N  --window T  --backend auto|wgc|dxgi  --adapter N  --fps N\n"
        "Output:   --out PATH|none  --seconds N  --codec hevc|h264  --cqp N  --cbr --bitrate N  --preset N\n"
        "          --bframes 0-4  --lookahead\n"
        "Scene:    --webcam  --webcam-device NAME  --webcam-pos P  --webcam-size N\n"
        "          --chroma RRGGBB  --chroma-layer webcam|capture\n"
        "          --browser URL|FILE  --browser-size WxH  --no-cursor\n"
        "Clips:    --replay  --replay-seconds N  --replay-prefix S  --hotkey F1..F24|none  --clip-at N\n"
        "Stream:   --platform NAME  --key KEY  --rtmp URL  --also-record PATH  --reconnect N\n"
        "Audio:    --no-audio  --no-desktop-audio  --mic  --mic-device NAME  --desktop-device NAME\n"
        "          --mic-gain X  --desktop-gain X  --gate DB  --comp DB  --comp-ratio R\n"
        "          --audio-bitrate N  --tracks 1|2\n\n"
        "During a run: P pause/resume, R save clip, Q stop.\n");
}

int wmain(int argc, wchar_t** argv) {
    // CEF subprocess entry (browser overlays spawn helper processes).
    {
        int cefRc = BrowserOverlay::ExecuteSubprocess(GetModuleHandleW(nullptr));
        if (cefRc >= 0) return cefRc;
    }

    Options o;
    bool missingValue = false;
    o.ui = (argc <= 1);

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto takeNext = [&](std::wstring& v) {
            if (i + 1 < argc)
                v = argv[++i];
            else
                missingValue = true;
        };
        auto takeInt = [&](int& v) {
            std::wstring s;
            takeNext(s);
            v = _wtoi(s.c_str());
        };
        auto takeU32 = [&](uint32_t& v) {
            std::wstring s;
            takeNext(s);
            v = (uint32_t)_wtoi(s.c_str());
        };
        auto takeF = [&](float& v) {
            std::wstring s;
            takeNext(s);
            v = (float)_wtof(s.c_str());
        };
        auto takeD = [&](double& v) {
            std::wstring s;
            takeNext(s);
            v = _wtof(s.c_str());
        };
        auto takeS = [&](std::string& v) {
            std::wstring s;
            takeNext(s);
            v = Narrow(s);
        };

        if (a == L"--ui") o.ui = true;
        else if (a == L"--list") o.list = true;
        else if (a == L"--list-audio") o.listAudio = true;
        else if (a == L"--diagnostics") o.diagnostics = true;
        else if (a == L"--open-logs") o.openLogs = true;
        else if (a == L"--version" || a == L"-V") o.version = true;
        else if (a == L"--twitch-login") o.twitchLogin = true;
        else if (a == L"--twitch-client-id") takeS(o.twitchClientId);
        else if (a == L"--monitor") takeInt(o.monitor);
        else if (a == L"--adapter") takeInt(o.adapter);
        else if (a == L"--backend") takeS(o.backend);
        else if (a == L"--window") takeNext(o.window);
        else if (a == L"--out") takeS(o.out);
        else if (a == L"--seconds") takeD(o.seconds);
        else if (a == L"--fps") takeU32(o.fps);
        else if (a == L"--codec") {
            std::wstring v;
            takeNext(v);
            std::wstring lv = v;
            for (auto& ch : lv) ch = (wchar_t)towlower(ch);
            if (lv != L"h264" && lv != L"hevc") {
                std::printf("ERROR: --codec expects hevc or h264\n");
                return 1;
            }
            o.hevc = (lv == L"hevc");
        }
        else if (a == L"--cqp") takeU32(o.cqp);
        else if (a == L"--cbr") o.cbr = true;
        else if (a == L"--bitrate") takeU32(o.bitrate);
        else if (a == L"--preset") takeInt(o.preset);
        else if (a == L"--bframes") takeInt(o.bframes);
        else if (a == L"--lookahead") o.lookahead = true;
        else if (a == L"--webcam") o.webcam = true;
        else if (a == L"--webcam-device") { takeNext(o.webcamDevice); o.webcam = true; }
        else if (a == L"--webcam-pos") takeS(o.webcamPos);
        else if (a == L"--webcam-size") takeInt(o.webcamWidth);
        else if (a == L"--replay") o.replay = true;
        else if (a == L"--replay-seconds") takeInt(o.replaySeconds);
        else if (a == L"--replay-prefix") takeS(o.replayPrefix);
        else if (a == L"--hotkey") { std::string s; takeS(s); o.hotkeyReplay = ParseHotkey(s); }
        else if (a == L"--clip-at") takeD(o.clipAt);
        else if (a == L"--chroma") { takeS(o.chromaColor); o.chroma = true; }
        else if (a == L"--chroma-layer") {
            std::wstring v;
            takeNext(v);
            o.chromaLayer = (v == L"capture") ? 1 : 0;
            o.chroma = true;
        }
        else if (a == L"--browser") takeS(o.browserUrl);
        else if (a == L"--no-cursor") o.showCursor = false;
        else if (a == L"--force-dshow") o.forceDshow = true;
        else if (a == L"--browser-size") {
            std::wstring v;
            takeNext(v);
            swscanf_s(v.c_str(), L"%dx%d", &o.browserWidth, &o.browserHeight);
        }
        else if (a == L"--no-audio") o.noAudio = true;
        else if (a == L"--no-desktop-audio") o.desktopAudio = false;
        else if (a == L"--mic") o.mic = true;
        else if (a == L"--mic-device") { takeNext(o.micDevice); o.mic = true; }
        else if (a == L"--desktop-device") takeNext(o.desktopDevice);
        else if (a == L"--mic-gain") takeF(o.micGain);
        else if (a == L"--desktop-gain") takeF(o.desktopGain);
        else if (a == L"--audio-bitrate") takeInt(o.audioBitrate);
        else if (a == L"--gate") { o.gate = true; takeInt(o.gateDb); }
        else if (a == L"--comp") { o.comp = true; takeInt(o.compDb); }
        else if (a == L"--comp-ratio") { int r = 30; takeInt(r); o.compRatioX10 = r; }
        else if (a == L"--monitor-mic") o.monitorMic = true;
        else if (a == L"--tracks") takeInt(o.tracks);
        else if (a == L"--platform") takeS(o.platform);
        else if (a == L"--key") takeS(o.streamKey);
        else if (a == L"--rtmp") takeS(o.rtmpUrl);
        else if (a == L"--also-record") takeS(o.alsoRecord);
        else if (a == L"--reconnect") takeInt(o.reconnect);
        else if (a == L"--pause-at") takeD(o.pauseAt);
        else if (a == L"--resume-at") takeD(o.resumeAt);
        else if (a == L"--help" || a == L"-h") { PrintUsage(); return 0; }
        else {
            std::printf("unknown argument: %ls\n\n", a.c_str());
            PrintUsage();
            return 1;
        }
    }

    SetConsoleOutputCP(CP_UTF8);
    av_log_set_level(16 /* AV_LOG_ERROR */);
    Diag::StartLog("cli");

    if (missingValue) {
        std::printf("ERROR: an option is missing its value\n\n");
        PrintUsage();
        return 1;
    }

    if (o.version) {
        std::printf("Ulticast " ULTI_VERSION "\n");
        return 0;
    }

    if (o.openLogs) {
        std::string dir = Diag::LogDir();
        ShellExecuteW(nullptr, L"open", Widen(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }

    if (o.twitchLogin) {
        // Headless device-code login: prints the code, waits for the browser
        // confirmation, then prints the channel and stream key.
        const std::string clientId =
            o.twitchClientId.empty() ? Twitch::kDefaultClientId : o.twitchClientId;
        Twitch::DeviceCode code;
        std::string err;
        if (!Twitch::RequestDeviceCode(clientId, &code, &err)) {
            std::printf("ERROR: %s\n", err.c_str());
            return 2;
        }
        std::printf("\n1. Open %s\n2. Enter this code: %s\n\nWaiting for confirmation...\n",
                    code.verificationUri.c_str(), code.userCode.c_str());
        std::fflush(stdout);  // the code must be visible while we poll
        ShellExecuteW(nullptr, L"open", Widen(code.verificationUri).c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);

        Twitch::Tokens tokens;
        int interval = code.interval;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(code.expiresIn);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            if (std::chrono::steady_clock::now() > deadline) {
                std::printf("ERROR: the code expired; run --twitch-login again\n");
                return 2;
            }
            const Twitch::Poll p = Twitch::PollToken(clientId, code, &tokens, &err);
            if (p == Twitch::Poll::Authorized) break;
            if (p == Twitch::Poll::Pending) continue;
            if (p == Twitch::Poll::SlowDown) {
                interval += 2;
                continue;
            }
            std::printf("ERROR: %s\n", err.c_str());
            return 2;
        }
        if (!Twitch::FetchUser(clientId, &tokens, &err)) {
            std::printf("ERROR: %s\n", err.c_str());
            return 2;
        }
        std::string key;
        if (!Twitch::FetchStreamKey(clientId, tokens, &key, &err)) {
            std::printf("ERROR: %s\n", err.c_str());
            return 2;
        }
        std::printf("Linked channel : %s\n", tokens.displayName.c_str());
        std::printf("Stream key     : %s\n", key.c_str());
        std::printf("\nThe UI stores this automatically when you press Connect Twitch there.\n");
        return 0;
    }

    if (o.diagnostics) {
        std::string report = Diag::BuildReport();
        std::string path = Diag::DataDir() + "\\diagnostics-" + [] {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            char buf[32];
            sprintf_s(buf, "%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                      st.wMinute, st.wSecond);
            return std::string(buf);
        }() + ".txt";
        std::ofstream f(path);
        if (f) f << report;
        std::printf("%s\n\n", report.c_str());
        std::printf("Saved to: %s\n", path.c_str());
        std::printf("Attach that file (and the log) to your bug report: " ULTI_REPO "/issues/new\n");
        return 0;
    }

    if (o.ui) {
        // Launch the UI executable that sits next to this one.
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir = exePath;
        size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir = dir.substr(0, slash);
        std::wstring ui = dir + L"\\Ulticast-ui.exe";
        HINSTANCE r = ShellExecuteW(nullptr, L"open", ui.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) {
            std::printf("ERROR: could not launch %s\n", Narrow(ui).c_str());
            return 1;
        }
        return 0;
    }

    if (o.list) {
        std::printf("Adapters:\n");
        for (auto& a : D3DContext::ListAdapters())
            std::printf("  [%d] %s\n", a.index, Narrow(a.name).c_str());
        auto monitors = WgcCapture::ListMonitors();
        std::printf("\nMonitors:\n");
        for (auto& m : monitors) {
            std::printf("  [%d] %ldx%ld at (%ld,%ld)%s  %s\n", m.index,
                        m.rect.right - m.rect.left, m.rect.bottom - m.rect.top, m.rect.left,
                        m.rect.top, m.primary ? " (primary)" : "", Narrow(m.device).c_str());
        }
        std::printf("\nVideo capture devices (webcams):\n");
        for (auto& d : WebcamCapture::ListDevices())
            std::printf("  %s\n", Narrow(d).c_str());
        auto windows = WgcCapture::ListWindows();
        std::printf("\nWindows (%zu, showing up to 40):\n", windows.size());
        for (size_t i = 0; i < windows.size() && i < 40; ++i)
            std::printf("  %s\n", Narrow(windows[i]).c_str());
        return 0;
    }

    if (o.listAudio) {
        std::printf("Render devices (desktop loopback picks one of these):\n");
        for (auto& d : WasapiCapture::ListRenderDevices())
            std::printf("  %s%s\n", Narrow(d.name).c_str(), d.isDefault ? "  [default]" : "");
        std::printf("\nCapture devices (microphones):\n");
        for (auto& d : WasapiCapture::ListCaptureDevices())
            std::printf("  %s%s\n", Narrow(d.name).c_str(), d.isDefault ? "  [default]" : "");
        return 0;
    }

    // ---- resolve output / streaming target --------------------------------
    std::string url = o.out.empty() ? "record.mkv" : o.out;
    bool streaming = false;
    uint32_t platformBitrate = 6000;

    if (!o.rtmpUrl.empty()) {
        url = o.rtmpUrl;
    } else if (!o.platform.empty()) {
        std::string p = o.platform;
        for (auto& c : p) c = (char)tolower((unsigned char)c);
        const PlatformPreset* preset = nullptr;
        for (auto& entry : kPlatforms)
            if (p == entry.name) preset = &entry;
        if (!preset) {
            std::printf("ERROR: unknown platform '%s'\n", o.platform.c_str());
            return 1;
        }
        if (std::string(preset->name) == "custom") {
            std::printf("ERROR: --platform custom needs --rtmp URL\n");
            return 1;
        }
        if (o.streamKey.empty()) {
            std::printf("ERROR: --key <stream key> is required with --platform\n");
            return 1;
        }
        url = std::string(preset->url) + "/" + o.streamKey;
        platformBitrate = preset->bitrate;
    }

    const bool mainOutput = (url != "none");
    streaming = mainOutput && (url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0);
    if (!mainOutput && !o.replay) {
        std::printf("ERROR: --out none disables recording and streaming; enable --replay\n");
        return 1;
    }
    if (o.bitrate == 0) o.bitrate = streaming ? platformBitrate : 6000;
    if (o.hotkeyReplay == -1) {
        std::printf("ERROR: --hotkey expects F1..F24 or none\n");
        return 1;
    }

    SessionConfig cfg;
    cfg.monitor = o.monitor;
    cfg.window = o.window;
    cfg.backend = o.backend;
    cfg.fps = o.fps;
    cfg.adapter = o.adapter;
    if (streaming) {
        cfg.record = !o.alsoRecord.empty();
        cfg.recordPath = o.alsoRecord;
        cfg.stream = true;
        cfg.streamUrl = url;
        cfg.streamBitrate = o.bitrate;
        cfg.reconnect = o.reconnect;
    } else if (mainOutput) {
        cfg.record = true;
        cfg.recordPath = url;
    } else {
        cfg.record = false;
        cfg.stream = false;
    }
    cfg.hevc = o.hevc;
    cfg.cqp = o.cqp;
    cfg.recordCbr = o.cbr;
    cfg.recordBitrate = o.bitrate ? o.bitrate : 12000;
    cfg.preset = o.preset;
    cfg.bframes = o.bframes;
    cfg.recordLookahead = o.lookahead;
    cfg.recordMultipass = false;
    cfg.streamPreset = o.preset;
    cfg.streamBframes = o.bframes;
    cfg.streamLookahead = o.lookahead;
    cfg.streamMultipass = false;
    cfg.noAudio = o.noAudio;
    cfg.audio.desktop = o.desktopAudio;
    cfg.audio.mic = o.mic;
    cfg.audio.desktopDevice = o.desktopDevice;
    cfg.audio.micDevice = o.micDevice;
    cfg.audio.desktopGain = o.desktopGain;
    cfg.audio.micGain = o.micGain;
    cfg.audio.gate = o.gate;
    cfg.audio.gateDb = (float)o.gateDb;
    cfg.audio.comp = o.comp;
    cfg.audio.compDb = (float)o.compDb;
    cfg.audio.compRatio = o.compRatioX10 / 10.0f;
    cfg.audio.monitor = o.monitorMic;
    cfg.audio.bitrateKbps = o.audioBitrate;
    cfg.audio.tracks = streaming ? 1 : o.tracks;
    cfg.webcam = o.webcam;
    cfg.webcamDevice = o.webcamDevice;
    cfg.webcamPos = o.webcamPos;
    cfg.webcamWidth = o.webcamWidth;
    cfg.replay = o.replay;
    cfg.replaySeconds = o.replaySeconds;
    cfg.replayPrefix = o.replayPrefix;
    cfg.chroma = o.chroma;
    cfg.chromaColor = o.chromaColor;
    cfg.chromaLayer = o.chromaLayer;
    cfg.browser = !o.browserUrl.empty();
    cfg.browserUrl = o.browserUrl;
    cfg.browserWidth = o.browserWidth;
    cfg.browserHeight = o.browserHeight;
    cfg.showCursor = o.showCursor;
    cfg.forceDshow = o.forceDshow;

    Session session;
    std::string err;
    if (!session.Start(cfg, &err)) {
        std::printf("ERROR: %s\n", err.c_str());
        BrowserOverlay::ShutdownGlobal();
        return 2;
    }

    const int kHotkeyId = 1;
    if (o.replay && o.hotkeyReplay != 0) {
        if (RegisterHotKey(nullptr, kHotkeyId, 0, o.hotkeyReplay))
            std::printf("[hotkey] global clip key registered (F%d)\n",
                        o.hotkeyReplay - VK_F1 + 1);
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    double lastStatEl = 0.0;
    bool hookClipSaved = false;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto st = session.GetStats();
        if (st.failed) {
            std::printf("ERROR: %s\n", st.error.c_str());
            break;
        }
        if (!st.running) break;
        if (o.seconds > 0 && st.elapsed >= o.seconds) break;

        if (o.clipAt >= 0 && !hookClipSaved && st.elapsed >= o.clipAt) {
            hookClipSaved = true;
            if (session.SaveClip()) std::printf("[replay] saving clip...\n");
        }

        if (o.pauseAt >= 0 && !streaming) {
            bool should = st.elapsed >= o.pauseAt && (o.resumeAt < 0 || st.elapsed < o.resumeAt);
            if (should != session.Paused()) {
                session.SetPaused(should);
                std::printf("[pause] %s\n", should ? "paused" : "resumed");
            }
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && (int)msg.wParam == kHotkeyId) session.SaveClip();
        }

        while (_kbhit()) {
            int c = _getch();
            if ((c == 'p' || c == 'P') && !streaming) {
                session.SetPaused(!session.Paused());
                std::printf("[pause] %s\n", session.Paused() ? "paused" : "resumed");
            } else if (c == 'r' || c == 'R') {
                if (session.SaveClip()) std::printf("[replay] saving clip...\n");
            } else if (c == 'q' || c == 'Q') {
                g_stop = true;
            }
        }

        if (st.elapsed - lastStatEl >= 1.0) {
            lastStatEl = st.elapsed;
            std::printf("[t=%6.1fs] cap %6llu | enc %6llu | %7.0f kbps%s%s%s\n", st.elapsed,
                        (unsigned long long)st.captured, (unsigned long long)st.encoded, st.kbps,
                        st.paused ? "  [PAUSED]" : "",
                        st.reconnecting ? "  [RECONNECTING]" : "",
                        st.clipsSaved > 0 ? "  [clips saved]" : "");
        }
    }
    g_stop = true;
    session.Stop();
    BrowserOverlay::ShutdownGlobal();

    auto st = session.GetStats();
    std::printf("\n--- results ---\n");
    std::printf("elapsed   : %.2f s\n", st.elapsed);
    std::printf("captured  : %llu frames\n", (unsigned long long)st.captured);
    std::printf("encoded   : %llu\n", (unsigned long long)st.encoded);
    std::printf("dropped   : %llu failed, %llu skipped while paused\n",
                (unsigned long long)st.failedFrames, (unsigned long long)st.skippedPaused);
    if (o.replay) std::printf("clips     : %d saved\n", st.clipsSaved);
    if (st.backend == "DXGI")
        std::printf("dxgi      : %llu pointer updates, %llu shape updates\n",
                    (unsigned long long)st.dxgiPointer, (unsigned long long)st.dxgiShapes);
    if (cfg.record) {
        HANDLE h = CreateFileW(Widen(cfg.recordPath).c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz = {};
            GetFileSizeEx(h, &sz);
            CloseHandle(h);
            std::printf("file size : %.1f MB\n", (double)sz.QuadPart / 1048576.0);
        }
    }
    return st.failed ? 4 : 0;
}
