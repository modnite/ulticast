#include "session.hpp"
#include "core/diag.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <winrt/base.h>

Session::~Session() {
    Stop();
}

double Session::ElapsedSeconds() const {
    if (running_.load() && !stop_.load())
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    return lastElapsed_.load();
}

void Session::Fail(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(errMu_);
        if (failErr_.empty()) failErr_ = msg;
    }
    Log("%s", msg.c_str());
    failed_ = true;
    stop_ = true;
}

void Session::Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    Diag::Log("%s", buf);
    if (logFn_) logFn_(buf);
}

bool Session::Start(const SessionConfig& cfg, std::string* err) {
    if (running_.load()) {
        *err = "session already running";
        return false;
    }
    cfg_ = cfg;

    stop_ = false;
    paused_ = false;
    recon_ = false;
    failures_ = 0;
    forceIdrRec_ = false;
    forceIdrStr_ = false;
    failed_ = false;
    inited_ = false;
    dxgiChecked_ = false;
    usingDxgi_ = false;
    encoded_ = 0;
    failedFrames_ = 0;
    skipped_ = 0;
    pacedFrames_ = 0;
    lastFrameTime_ = {};
    vidOut_ = 0;
    replayFrames_ = 0;
    framesSeen_ = 0;
    previewVersion_ = 0;
    kbps_ = 0.0;
    {
        std::lock_guard<std::mutex> lock(errMu_);
        failErr_.clear();
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    if (!d3d_.Create(err, cfg_.adapter)) return false;

    if (cfg_.sources.empty()) {
        SessionSource cap;
        cap.type = SessionSourceType::Capture;
        cfg_.sources.push_back(cap);
        if (cfg_.webcam) {
            SessionSource w;
            w.type = SessionSourceType::Webcam;
            w.device = Narrow(cfg_.webcamDevice);
            if (cfg_.webcamPos == "topleft") w.corner = 0;
            else if (cfg_.webcamPos == "topright") w.corner = 1;
            else if (cfg_.webcamPos == "bottomleft") w.corner = 2;
            else w.corner = 3;
            cfg_.sources.push_back(w);
        }
        if (cfg_.browser) {
            SessionSource b;
            b.type = SessionSourceType::Browser;
            cfg_.sources.push_back(b);
        }
    }
    {
        std::lock_guard<std::mutex> lk(sourcesMu_);
        liveSources_ = cfg_.sources;
        liveWebcamPos_ = cfg_.webcamPos;
    }
    showCursorLive_.store(cfg_.showCursor);

    // Live output state: the UI can switch recording/streaming/replay on and
    // off at runtime; Start() only sets the initial state.
    recOn_ = cfg_.record;
    strOn_ = cfg_.stream;
    recBaseFrames_ = strBaseFrames_ = 0;
    recBaseAudio_ = strBaseAudio_ = 0;
    lastAudioPts_ = 0;
    {
        std::lock_guard<std::mutex> lk(cmdMu_);
        cmds_.clear();
    }

    // Chroma key color (RRGGBB -> floats)
    keyR_ = 0.f;
    keyG_ = 1.f;
    keyB_ = 0.f;
    if (cfg_.chroma) {
        unsigned r = 0, g = 0, b = 0;
        if (sscanf_s(cfg_.chromaColor.c_str(), "%02x%02x%02x", &r, &g, &b) != 3) {
            *err = "chroma color must be 6 hex digits (RRGGBB)";
            return false;
        }
        keyR_ = r / 255.f;
        keyG_ = g / 255.f;
        keyB_ = b / 255.f;
    }

    // Audio devices first so both timelines start cleanly.
    audioReady_ = false;
    if (!cfg_.noAudio && (cfg_.audio.desktop || cfg_.audio.mic)) {
        std::string e;
        if (!mixer_.Prepare(cfg_.audio, 48000, &e)) {
            Log("[audio] disabled: %s", e.c_str());
        } else if (!mixer_.StartDevices(&e)) {
            Log("[audio] disabled: %s", e.c_str());
        } else {
            audioReady_ = true;
        }
    }
    mixer_.SetPausedFn([this]() { return paused_.load(); });

    FrameCallback cb = [this](ID3D11Texture2D* t, uint32_t w, uint32_t h, uint64_t i) {
        OnFrame(t, w, h, i);
    };

    std::string backend = cfg_.backend;
    for (auto& c : backend) c = (char)tolower((unsigned char)c);
    // "auto" used to try DXGI duplication first — that hooks fullscreen games
    // without asking. It now means plain desktop/window capture (WGC); DXGI is
    // only used when the user explicitly picks it.
    if (backend.empty() || backend == "auto") backend = "wgc";

    if (cfg_.windowSource && cfg_.window.empty()) {
        // A window source without a chosen window must never fall back to
        // capturing the whole monitor -- that is a different source entirely.
        *err = "window source has no window selected";
        Log("[capture] %s", err->c_str());
        Stop();
        return false;
    }
    if (cfg_.windowSource || !cfg_.window.empty()) {
        usingDxgi_ = false;
        if (!cap_.StartWindow(cfg_.window, d3d_.device.Get(), cb, err, cfg_.showCursor)) {
            Stop();
            return false;
        }
    } else if (backend == "wgc") {
        usingDxgi_ = false;
        if (!cap_.StartMonitor(cfg_.monitor, d3d_.device.Get(), cb, err, cfg_.showCursor)) {
            Stop();
            return false;
        }
    } else {
        usingDxgi_ = true;
        const bool requireContent = (backend != "dxgi");
        if (!dup_.Start(cfg_.monitor, d3d_.device.Get(), cfg_.fps, cb, requireContent, err)) {
            if (backend == "dxgi") {
                Stop();
                return false;
            }
            Log("[capture] DXGI unavailable (%s); falling back to WGC", err->c_str());
            usingDxgi_ = false;
            if (!cap_.StartMonitor(cfg_.monitor, d3d_.device.Get(), cb, err, cfg_.showCursor)) {
                Stop();
                return false;
            }
        }
    }

    StartVideoSources();
    StartExtraCaptures();

    t0_ = std::chrono::steady_clock::now();
    lastElapsed_ = 0.0;
    // Output timers: the UI shows these, not the session age (the idle preview
    // is a session too, and it must not tick a recording timer).
    recStart_ = cfg_.record ? t0_ : std::chrono::steady_clock::time_point{};
    strStart_ = cfg_.stream ? t0_ : std::chrono::steady_clock::time_point{};
    running_ = true;
    supervisor_ = std::thread(&Session::Supervisor, this);
    return true;
}

void Session::Stop() {
    bool wasRunning = running_.exchange(false);
    stop_ = true;
    {
        std::lock_guard<std::mutex> lk(cmdMu_);
        cmds_.clear();
    }
    recOn_ = false;
    strOn_ = false;
    if (wasRunning)
        lastElapsed_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();

    // Join the supervisor first: it may be inside a reconnect or the DXGI->WGC
    // fallback touching the same capture/muxer objects we are about to tear down.
    if (supervisor_.joinable()) supervisor_.join();

    cap_.Stop();
    StopExtraCaptures();
    mixer_.Stop();
    dup_.Stop();
    for (auto& v : videos_) {
        if (v.cam) v.cam->Stop();
        v.cam.reset();
        v.tex.Reset();
        v.srv.Reset();
    }
    videos_.clear();
    webcamOn_ = false;
    browser_.Stop();
    browserOn_ = false;
    recMux_.Close();
    strMux_.Close();
    replay_.Stop();
    recEnc_.Shutdown();
    strEnc_.Shutdown();
    {
        std::lock_guard<std::mutex> lock(previewMu_);
        previewTex_.Reset();
    }
}

void Session::SetPaused(bool p) {
    if (p == paused_.load()) return;
    paused_ = p;
    if (!p) {
        forceIdrRec_ = true;
        forceIdrStr_ = true;
    }
}

void Session::SetSources(std::vector<SessionSource> sources, std::string webcamPos) {
    std::lock_guard<std::mutex> lk(sourcesMu_);
    liveSources_ = std::move(sources);
    liveWebcamPos_ = std::move(webcamPos);
    transitionPending_ = true;  // snapshot the outgoing frame on the capture thread
}

// ------------------------------------------------------------ runtime outputs

void Session::EnableRecording(const std::string& path) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    OutCmd c;
    c.type = OutCmdType::RecOn;
    c.arg = path;
    cmds_.push_back(std::move(c));
}

void Session::DisableRecording() {
    std::lock_guard<std::mutex> lk(cmdMu_);
    OutCmd c;
    c.type = OutCmdType::RecOff;
    cmds_.push_back(std::move(c));
}

void Session::EnableStreaming(const std::string& url, uint32_t bitrateKbps) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    OutCmd c;
    c.type = OutCmdType::StrOn;
    c.arg = url;
    c.bitrate = bitrateKbps;
    cmds_.push_back(std::move(c));
}

void Session::DisableStreaming() {
    std::lock_guard<std::mutex> lk(cmdMu_);
    OutCmd c;
    c.type = OutCmdType::StrOff;
    cmds_.push_back(std::move(c));
}

void Session::EnableReplay(int seconds, const std::string& prefix) {
    std::lock_guard<std::mutex> lk(cmdMu_);
    OutCmd c;
    c.type = OutCmdType::RepOn;
    c.arg = prefix;
    c.seconds = seconds;
    cmds_.push_back(std::move(c));
}

void Session::DisableReplay() {
    std::lock_guard<std::mutex> lk(cmdMu_);
    OutCmd c;
    c.type = OutCmdType::RepOff;
    cmds_.push_back(std::move(c));
}

void Session::StartVideoSources() {
    videos_.clear();
    for (const auto& s : cfg_.sources) {
        if (s.type != SessionSourceType::Webcam || !s.enabled) continue;
        VideoLayer v;
        v.device = s.device;
        const std::wstring match = s.device.empty() ? L"default" : Widen(s.device);
        std::string eMf, eDs;

        auto tryMf = [&]() -> bool {
            auto cam = std::make_unique<WebcamCapture>();
            if (cam->Start(match, &eMf)) {
                v.cam = std::move(cam);
                return true;
            }
            return false;
        };
        auto tryDs = [&]() -> bool {
            auto cam = std::make_unique<DshowCapture>();
            if (cam->Start(match, &eDs)) {
                v.cam = std::move(cam);
                return true;
            }
            return false;
        };

        // Media Foundation first (shared-mode, lower latency); DirectShow as the
        // fallback for virtual cameras that only register with DirectShow.
        bool ok = cfg_.forceDshow ? (tryDs() || tryMf()) : (tryMf() || tryDs());
        if (ok) {
            Log("[video] %ls%s", v.cam->ActiveDevice().c_str(),
                v.cam->IsDirectShow() ? " (DirectShow)" : "");
        } else {
            Log("[video] disabled (%s): MF: %s | DirectShow: %s",
                s.device.empty() ? "default" : s.device.c_str(), eMf.c_str(), eDs.c_str());
        }
        videos_.push_back(std::move(v));
    }
    webcamOn_ = false;
    for (auto& v : videos_)
        if (v.cam) webcamOn_ = true;
}

Session::VideoLayer* Session::FindVideo(const std::string& device) {
    for (auto& v : videos_) {
        if (v.cam && !device.empty() && v.device == device) return &v;
    }
    if (device.empty()) {
        for (auto& v : videos_)
            if (v.cam) return &v;
    }
    return nullptr;
}

void Session::StartExtraCaptures() {
    int captureIdx = 0;
    for (const auto& s : cfg_.sources) {
        if (s.type != SessionSourceType::Capture && s.type != SessionSourceType::Window) continue;
        if (!s.enabled) continue;
        ++captureIdx;
        if (captureIdx == 1) continue;  // the first capture drives the frame tick
        if (s.type == SessionSourceType::Window && s.window.empty()) {
            // No window chosen yet: skip the layer instead of capturing a monitor.
            Log("[capture] extra window layer skipped: no window selected");
            continue;
        }

        auto e = std::make_unique<ExtraCapture>();
        e->monitor = s.monitor;
        e->window = s.window;
        ExtraCapture* ep = e.get();
        e->cap = std::make_unique<WgcCapture>();
        FrameCallback cb = [ep](ID3D11Texture2D* t, uint32_t, uint32_t, uint64_t) {
            std::lock_guard<std::mutex> lk(ep->mu);
            ep->pending = t;  // AddRef via ComPtr: the frame outlives the callback
        };
        std::string err;
        const bool isWindow = s.type == SessionSourceType::Window || !e->window.empty();
        const bool ok = isWindow
                            ? e->cap->StartWindow(e->window, d3d_.device.Get(), cb, &err,
                                                  cfg_.showCursor)
                            : e->cap->StartMonitor(e->monitor, d3d_.device.Get(), cb, &err,
                                                   cfg_.showCursor);
        if (!ok) {
            Log("[capture] extra layer failed: %s", err.c_str());
            continue;
        }
        Log("[capture] extra layer: %s", isWindow ? "window" : "monitor");
        extraCaps_.push_back(std::move(e));
    }
}

void Session::StopExtraCaptures() {
    for (auto& e : extraCaps_) {
        if (e->cap) e->cap->Stop();
        std::lock_guard<std::mutex> lk(e->mu);
        e->pending.Reset();
        e->tex.Reset();
        e->srv.Reset();
    }
    extraCaps_.clear();
}

void Session::SetCursorVisible(bool on) {
    showCursorLive_.store(on);
    cap_.SetCursorEnabled(on);
    for (auto& e : extraCaps_)
        if (e->cap) e->cap->SetCursorEnabled(on);
}

void Session::ApplyOutputCommands() {
    std::deque<OutCmd> todo;
    {
        std::lock_guard<std::mutex> lk(cmdMu_);
        todo.swap(cmds_);
    }
    for (auto& c : todo) {
        switch (c.type) {
            case OutCmdType::RecOn: BeginRecording(c.arg); break;
            case OutCmdType::RecOff: EndRecording(); break;
            case OutCmdType::StrOn: BeginStreaming(c.arg, c.bitrate); break;
            case OutCmdType::StrOff: EndStreaming(); break;
            case OutCmdType::RepOn: BeginReplay(c.seconds, c.arg); break;
            case OutCmdType::RepOff: EndReplay(); break;
        }
    }
}

bool Session::BeginRecording(const std::string& path) {
    if (recOn_.load()) return true;
    if (!inited_.load() || !tex_) {
        Log("[record] not ready yet");
        return false;
    }
    const uint32_t w = width_.load(), h = height_.load();
    EncoderConfig rc = MakeEncoderConfig(w, h);
    rc.hevc = cfg_.hevc;
    rc.rateMode = cfg_.recordCbr ? EncoderConfig::RateMode::CBR : EncoderConfig::RateMode::CQ;
    rc.cqp = cfg_.cqp;
    rc.bitrateKbps = cfg_.recordBitrate;
    rc.maxBitrateKbps = cfg_.recordBitrate;
    rc.lookahead = cfg_.recordLookahead;
    rc.multipass = cfg_.recordMultipass;

    std::string e;
    if (!recEnc_.Init(d3d_.device.Get(), rc, &e)) {
        Log("[record] start failed: %s", e.c_str());
        return false;
    }
    if (!recEnc_.RegisterTexture(tex_.Get(), &e)) {
        Log("[record] register failed: %s", e.c_str());
        recEnc_.Shutdown();
        return false;
    }
    recCfg_.width = (int)w;
    recCfg_.height = (int)h;
    recCfg_.fps = cfg_.fps;
    recCfg_.hevc = cfg_.hevc;
    recCfg_.videoExtradata = recEnc_.SequenceHeader();
    recCfg_.audioTracks = 0;
    if (audioReady_ && mixer_.AnyActive() && cfg_.audio.tracks > 0) {
        recCfg_.audioTracks = (cfg_.audio.tracks == 2 && mixer_.MicActive()) ? 2 : 1;
        recCfg_.audioExtradata = mixer_.MixExtradata();
    }
    const std::string out = path.empty() ? cfg_.recordPath : path;
    if (!recMux_.Open(out, recCfg_, &e)) {
        Log("[record] open failed: %s", e.c_str());
        recEnc_.Shutdown();
        return false;
    }
    // Fresh output: rebase video/audio timestamps so the file starts at zero.
    recBaseFrames_ = (int64_t)vidOut_.load();
    recBaseAudio_ = lastAudioPts_.load();
    forceIdrRec_ = true;
    recOn_ = true;
    recStart_ = std::chrono::steady_clock::now();
    Log("[record] %s", out.c_str());
    return true;
}

void Session::EndRecording() {
    if (!recOn_.load()) return;
    recOn_ = false;
    recStart_ = {};
    recMux_.Close();
    recEnc_.Shutdown();
    Log("[record] stopped");
}

bool Session::BeginStreaming(const std::string& url, uint32_t bitrateKbps) {
    if (strOn_.load()) return true;
    if (!inited_.load() || !tex_) {
        Log("[stream] not ready yet");
        return false;
    }
    const uint32_t w = width_.load(), h = height_.load();
    EncoderConfig sc = MakeEncoderConfig(w, h);
    sc.hevc = cfg_.streamHevc;
    sc.rateMode = EncoderConfig::RateMode::CBR;
    sc.bitrateKbps = bitrateKbps ? bitrateKbps : cfg_.streamBitrate;
    sc.maxBitrateKbps = sc.bitrateKbps;
    sc.preset = cfg_.streamPreset;
    sc.bframes = cfg_.streamBframes;
    sc.lookahead = cfg_.streamLookahead;
    sc.multipass = cfg_.streamMultipass;

    std::string e;
    if (!strEnc_.Init(d3d_.device.Get(), sc, &e)) {
        Log("[stream] encoder failed: %s", e.c_str());
        return false;
    }
    if (!strEnc_.RegisterTexture(tex_.Get(), &e)) {
        Log("[stream] register failed: %s", e.c_str());
        strEnc_.Shutdown();
        return false;
    }
    strCfg_.width = (int)w;
    strCfg_.height = (int)h;
    strCfg_.fps = cfg_.fps;
    strCfg_.hevc = cfg_.streamHevc;
    strCfg_.videoExtradata = strEnc_.SequenceHeader();
    strCfg_.audioTracks = 0;
    if (audioReady_ && mixer_.AnyActive()) {
        strCfg_.audioTracks = 1;
        strCfg_.audioExtradata = mixer_.MixExtradata();
    }
    const std::string target = url.empty() ? cfg_.streamUrl : url;
    bool opened = false;
    for (int a = 1; a <= 3 && !stop_.load(); ++a) {
        if (strMux_.Open(target, strCfg_, &e)) {
            opened = true;
            break;
        }
        Log("[stream] open failed: %s", e.c_str());
        if (a < 3) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!opened) {
        strEnc_.Shutdown();
        return false;
    }
    strBaseFrames_ = (int64_t)vidOut_.load();
    strBaseAudio_ = lastAudioPts_.load();
    forceIdrStr_ = true;
    failures_ = 0;
    recon_ = false;
    strOn_ = true;
    strStart_ = std::chrono::steady_clock::now();
    Log("[stream] %s", target.c_str());
    return true;
}

void Session::EndStreaming() {
    if (!strOn_.load()) return;
    strOn_ = false;
    strStart_ = {};
    recon_ = false;
    strMux_.Close();
    strEnc_.Shutdown();
    Log("[stream] stopped");
}

bool Session::BeginReplay(int seconds, const std::string& prefix) {
    if (replay_.Enabled()) return true;
    if (!inited_.load() || !tex_) {
        Log("[replay] not ready yet");
        return false;
    }
    const int secs = seconds > 0 ? seconds : cfg_.replaySeconds;
    EncoderConfig rc = MakeEncoderConfig(width_.load(), height_.load());
    rc.hevc = true;
    rc.rateMode = EncoderConfig::RateMode::CQ;
    rc.cqp = cfg_.cqp;
    std::vector<uint8_t> audioExtra =
        (audioReady_ && mixer_.AnyActive()) ? mixer_.MixExtradata() : std::vector<uint8_t>{};
    std::string e;
    if (!replay_.Init(d3d_.device.Get(), rc, secs, prefix.empty() ? cfg_.replayPrefix : prefix,
                      audioExtra, &e)) {
        Log("[replay] failed: %s", e.c_str());
        return false;
    }
    if (!replay_.RegisterTexture(tex_.Get(), &e)) {
        Log("[replay] register failed: %s", e.c_str());
        replay_.Disable();
        return false;
    }
    Log("[replay] armed (%ds)", secs);
    return true;
}

void Session::EndReplay() {
    if (!replay_.Enabled()) return;
    replay_.Stop();
    Log("[replay] disarmed");
}

bool Session::SaveClip() {
    if (!replay_.Enabled()) return false;
    return replay_.Save() == 0;
}

void Session::Supervisor() {
    auto lastT = std::chrono::steady_clock::now();
    uint64_t lastBytes = 0;

    while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stop_.load()) break;

        // Late DXGI content check (covers entry via a black first frame path).
        if (usingDxgi_.load() && !dxgiChecked_.load() && ElapsedSeconds() >= 2.0) {
            dxgiChecked_ = true;
            if (dup_.newFrames.load() == 0 && cfg_.backend != "dxgi") {
                dup_.Stop();
                FrameCallback cb = [this](ID3D11Texture2D* t, uint32_t w, uint32_t h, uint64_t i) {
                    OnFrame(t, w, h, i);
                };
                std::string e;
                if (cap_.StartMonitor(cfg_.monitor, d3d_.device.Get(), cb, &e)) {
                    usingDxgi_ = false;
                    Log("[capture] DXGI delivered no frames; switched to WGC");
                } else {
                    Fail("DXGI fallback to WGC failed: " + e);
                }
            }
        }

        if (recon_.load() && strOn_.load() && !failed_.load()) {
            if (!DoReconnect() && !stop_.load()) Fail("stream reconnect failed after all attempts");
        }

        auto nowT = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(nowT - lastT).count();
        if (dt >= 1.0) {
            uint64_t bytes = recMux_.BytesWritten() + strMux_.BytesWritten();
            if (bytes >= lastBytes)
                kbps_ = (double)(bytes - lastBytes) * 8.0 / dt / 1000.0;
            lastBytes = bytes;
            lastT = nowT;
        }
    }
}

bool Session::DoReconnect() {
    recon_ = true;
    strMux_.Close();
    for (int attempt = 1; attempt <= cfg_.reconnect && !stop_.load(); ++attempt) {
        int delayMs = (std::min)(2000 * attempt, 10000);
        Log("[stream] reconnecting (%d/%d) in %.0fs ...", attempt, cfg_.reconnect,
                    delayMs / 1000.0);
        for (int i = 0; i < delayMs / 100 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stop_.load()) return true;  // user stopped: not a reconnect failure
        std::string e;
        if (strMux_.Open(cfg_.streamUrl, strCfg_, &e)) {
            Log("[stream] reconnected");
            forceIdrStr_ = true;
            failures_ = 0;
            recon_ = false;
            return true;
        }
        Log("[stream] reconnect failed: %s", e.c_str());
    }
    return false;
}

EncoderConfig Session::MakeEncoderConfig(uint32_t w, uint32_t h) const {
    EncoderConfig base;
    base.width = w;
    base.height = h;
    base.fps = cfg_.fps;
    base.preset = cfg_.preset;
    base.bframes = cfg_.bframes;
    return base;
}

bool Session::InitPipeline(uint32_t w, uint32_t h, std::string* err) {
    // Encoders need even dimensions (4:2:0); window sizes can be odd, so the
    // canvas is rounded down and odd frames are drawn scaled into it.
    if ((w & 1) || (h & 1)) {
        Log("[capture] rounding %ux%u canvas to even", w, h);
        w &= ~1u;
        h &= ~1u;
    }
    if (w < 16 || h < 16) {
        *err = "capture size too small";
        return false;
    }
    width_ = w;
    height_ = h;

    EncoderConfig base = MakeEncoderConfig(w, h);

    std::string e;
    if (cfg_.record) {
        EncoderConfig rc = base;
        rc.hevc = cfg_.hevc;
        rc.rateMode =
            cfg_.recordCbr ? EncoderConfig::RateMode::CBR : EncoderConfig::RateMode::CQ;
        rc.cqp = cfg_.cqp;
        rc.bitrateKbps = cfg_.recordBitrate;
        rc.maxBitrateKbps = cfg_.recordBitrate;
        rc.lookahead = cfg_.recordLookahead;
        rc.multipass = cfg_.recordMultipass;
        if (!recEnc_.Init(d3d_.device.Get(), rc, &e)) {
            *err = "record encoder: " + e;
            return false;
        }
    }
    if (cfg_.stream) {
        EncoderConfig sc = base;
        sc.hevc = cfg_.streamHevc;
        sc.rateMode = EncoderConfig::RateMode::CBR;  // streaming wants a predictable bitrate
        sc.bitrateKbps = cfg_.streamBitrate;
        sc.maxBitrateKbps = cfg_.streamBitrate;
        sc.preset = cfg_.streamPreset;
        sc.bframes = cfg_.streamBframes;
        sc.lookahead = cfg_.streamLookahead;
        sc.multipass = cfg_.streamMultipass;
        if (!strEnc_.Init(d3d_.device.Get(), sc, &e)) {
            *err = "stream encoder: " + e;
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = d3d_.device->CreateTexture2D(&td, nullptr, tex_.GetAddressOf());
    if (FAILED(hr)) {
        *err = "CreateTexture2D failed";
        return false;
    }
    hr = d3d_.device->CreateTexture2D(&td, nullptr, sceneTex_.GetAddressOf());
    if (FAILED(hr)) {
        *err = "scene texture failed";
        return false;
    }
    sceneSrv_ = Compositor::CreateSrv(d3d_.device.Get(), sceneTex_.Get());
    if (!sceneSrv_) {
        *err = "scene SRV failed";
        return false;
    }
    if (FAILED(d3d_.device->CreateRenderTargetView(tex_.Get(), nullptr, rtv_.GetAddressOf()))) {
        *err = "RTV failed";
        return false;
    }
    if (!compositor_.Init(d3d_.device.Get(), d3d_.context.Get(), w, h, &e)) {
        *err = "compositor: " + e;
        return false;
    }

    // Preview texture shared with the UI process-side device.
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    {
        std::lock_guard<std::mutex> lock(previewMu_);
        previewTex_.Reset();
        hr = d3d_.device->CreateTexture2D(&td, nullptr, previewTex_.GetAddressOf());
        if (FAILED(hr)) previewTex_.Reset();  // preview is optional
    }

    if (cfg_.record && !recEnc_.RegisterTexture(tex_.Get(), &e)) {
        *err = "record register: " + e;
        return false;
    }
    if (cfg_.stream && !strEnc_.RegisterTexture(tex_.Get(), &e)) {
        *err = "stream register: " + e;
        return false;
    }

    if (cfg_.replay) {
        EncoderConfig rc = base;
        rc.hevc = true;
        rc.rateMode = EncoderConfig::RateMode::CQ;
        rc.cqp = cfg_.cqp;
        std::vector<uint8_t> audioExtra =
            (audioReady_ && mixer_.AnyActive()) ? mixer_.MixExtradata() : std::vector<uint8_t>{};
        if (!replay_.Init(d3d_.device.Get(), rc, cfg_.replaySeconds, cfg_.replayPrefix, audioExtra,
                          &e)) {
            Log("[replay] disabled: %s", e.c_str());
        } else if (!replay_.RegisterTexture(tex_.Get(), &e)) {
            Log("[replay] register failed: %s", e.c_str());
            replay_.Disable();
        }
    }

    if (cfg_.browser && !cfg_.browserUrl.empty()) {
        std::string e2;
        int bw = cfg_.browserWidth > 0 ? cfg_.browserWidth : (int)w;
        int bh = cfg_.browserHeight > 0 ? cfg_.browserHeight : (int)h;
        // Load status reaches the activity strip, so a blank overlay is never silent.
        browser_.SetStatusFn([this](const std::string& m) { Log("[browser] %s", m.c_str()); });
        if (!browser_.Start(cfg_.browserUrl, bw, bh, &e2)) {
            Log("[browser] disabled: %s", e2.c_str());
        } else {
            browserOn_ = true;
            Log("[browser] overlay: %s (%dx%d)", cfg_.browserUrl.c_str(), bw, bh);
        }
    }

    if (cfg_.record) {
        recCfg_.width = (int)w;
        recCfg_.height = (int)h;
        recCfg_.fps = cfg_.fps;
        recCfg_.hevc = cfg_.hevc;
        recCfg_.videoExtradata = recEnc_.SequenceHeader();
        if (audioReady_ && mixer_.AnyActive() && cfg_.audio.tracks > 0) {
            recCfg_.audioTracks =
                (cfg_.audio.tracks == 2 && mixer_.MicActive()) ? 2 : 1;
            recCfg_.audioExtradata = mixer_.MixExtradata();
        }
        if (!recMux_.Open(cfg_.recordPath, recCfg_, &e)) {
            *err = "record open: " + e;
            return false;
        }
        Log("[record] %s", cfg_.recordPath.c_str());
    }

    if (cfg_.stream) {
        strCfg_.width = (int)w;
        strCfg_.height = (int)h;
        strCfg_.fps = cfg_.fps;
        strCfg_.hevc = cfg_.streamHevc;
        strCfg_.videoExtradata = strEnc_.SequenceHeader();
        if (audioReady_ && mixer_.AnyActive()) {
            strCfg_.audioTracks = 1;
            strCfg_.audioExtradata = mixer_.MixExtradata();
        }
        bool opened = false;
        for (int a = 1; a <= 3 && !stop_.load(); ++a) {
            if (strMux_.Open(cfg_.streamUrl, strCfg_, &e)) {
                opened = true;
                break;
            }
            Log("[stream] open failed: %s", e.c_str());
            if (a < 3) std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        if (!opened) {
            *err = "stream open: " + e;
            return false;
        }
    }

    if (audioReady_ && mixer_.AnyActive())
        mixer_.AttachSink([this](int track, const uint8_t* d, size_t s, int64_t p) {
            return OnAudio(track, d, s, p);
        });

    inited_ = true;
    return true;
}

void Session::OnFrame(ID3D11Texture2D* src, uint32_t fw, uint32_t fh, uint64_t fidx) {
    (void)fidx;
    if (stop_.load()) return;

    if (!inited_.load()) {
        std::string e;
        if (!InitPipeline(fw, fh, &e)) {
            Fail(e);
            return;
        }
        lastFrameTime_ = std::chrono::steady_clock::now();
    }
    if (fw != width_.load() || fh != height_.load()) {
        // A window can be resized (or be odd-sized) while the canvas stays put:
        // draw the frame scaled instead of failing the session.
        if (!inited_.load()) {
            Fail("capture changed size before the pipeline was ready");
            return;
        }
    }

    // Outputs can be switched on/off at any time; do it between frames.
    ApplyOutputCommands();

    // Scene crossfade: snapshot the outgoing composite the first frame after a
    // scene switch, then blend it out over ~250 ms (the compositor's per-layer
    // opacity does the blend).
    float fadeAlpha = 0.f;
    if (transitionPending_.exchange(false) && cfg_.sceneFade) {
        if (!transitionTex_) {
            D3D11_TEXTURE2D_DESC td{};
            tex_->GetDesc(&td);
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = 0;
            d3d_.device->CreateTexture2D(&td, nullptr, transitionTex_.GetAddressOf());
        }
        if (transitionTex_) {
            // Snapshot the previous *composited* frame (tex_ still holds it) so
            // the fade blends the outgoing picture, overlays included.
            d3d_.context->CopyResource(transitionTex_.Get(), tex_.Get());
            if (!transitionSrv_)
                transitionSrv_ = Compositor::CreateSrv(d3d_.device.Get(), transitionTex_.Get());
            transitionStart_ = std::chrono::steady_clock::now();
            Log("[scene] crossfade 250 ms");
        }
    }
    if (transitionTex_ && cfg_.sceneFade) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                      transitionStart_)
                             .count();
        constexpr double kFadeSeconds = 0.25;
        if (t < kFadeSeconds) fadeAlpha = (float)(1.0 - t / kFadeSeconds);
        else {
            transitionTex_.Reset();
            transitionSrv_.Reset();
        }
    }

    // Pace to the configured frame rate: WGC can deliver at the display rate,
    // which would otherwise stretch the frame-counted media clock.
    {
        auto now = std::chrono::steady_clock::now();
        const double interval = 1.0 / (double)(cfg_.fps ? cfg_.fps : 60);
        if (std::chrono::duration<double>(now - lastFrameTime_).count() < interval * 0.9) return;
        lastFrameTime_ = now;
        pacedFrames_.fetch_add(1);
    }

    // ---- scene ----
    const uint32_t canvasU32W = width_.load();
    const uint32_t canvasU32H = height_.load();
    // Primary capture -> canvas: a straight copy when the sizes match, otherwise
    // draw the frame scaled (letterboxed) so odd window sizes and mid-session
    // window resizes still work.
    scaledFrameSrv_.Reset();
    if (fw == canvasU32W && fh == canvasU32H) {
        d3d_.context->CopyResource(sceneTex_.Get(), src);
    } else {
        scaledFrameSrv_ = Compositor::CreateSrv(d3d_.device.Get(), src);
        if (scaledFrameSrv_ && !sizeScaled_) {
            sizeScaled_ = true;
            Log("[capture] scaling %ux%u into the %ux%u canvas", fw, fh, canvasU32W, canvasU32H);
        }
    }

    LayerDraw layers[8];
    int layerCount = 0;
    auto applyKey = [&](LayerDraw& l) {
        l.key = true;
        l.keyR = keyR_;
        l.keyG = keyG_;
        l.keyB = keyB_;
    };
    auto pushLayer = [&](ID3D11ShaderResourceView* srv, float x, float y, float w, float h) {
        if (layerCount < 8) layers[layerCount++] = {srv, x, y, w, h, 1.0f};
    };
    const float canvasW = (float)width_.load();
    const float canvasH = (float)height_.load();

    std::vector<SessionSource> sources;
    std::string webcamPos;
    {
        std::lock_guard<std::mutex> lk(sourcesMu_);
        sources = liveSources_;
        webcamPos = liveWebcamPos_;
    }

    int captureIdx = 0;
    // Orientation (flip / quarter turns) is applied to whichever layer was pushed
    // last, in the compositor's vertex shader.
    auto applyXform = [&](const SessionSource& s) {
        if (layerCount <= 0) return;
        layers[layerCount - 1].flipX = s.flipH;
        layers[layerCount - 1].flipY = s.flipV;
        layers[layerCount - 1].rotate = s.rotate & 3;
    };
    for (const auto& s : sources) {
        if (!s.enabled || layerCount >= 8) continue;

        if (s.type == SessionSourceType::Capture || s.type == SessionSourceType::Window) {
            const int ci = captureIdx++;
            if (ci == 0) {
                ID3D11ShaderResourceView* base =
                    scaledFrameSrv_ ? scaledFrameSrv_.Get() : sceneSrv_.Get();
                float x = 0.f, y = 0.f, w = (float)canvasW, h = (float)canvasH;
                if (scaledFrameSrv_) {
                    // letterbox: keep the captured frame's aspect ratio
                    const float fa = (float)fw / (float)fh;
                    const float ca = (float)canvasW / (float)canvasH;
                    if (fa > ca) {
                        w = (float)canvasW;
                        h = (float)canvasW / fa;
                        y = ((float)canvasH - h) / 2.f;
                    } else {
                        h = (float)canvasH;
                        w = (float)canvasH * fa;
                        x = ((float)canvasW - w) / 2.f;
                    }
                } else if (s.w > 0) {
                    x = (float)s.x;
                    y = (float)s.y;
                    w = (float)s.w;
                    h = (float)s.h;
                }
                pushLayer(base, x, y, w, h);
                applyXform(s);
                // CLI path: the primary capture's orientation can come from the
                // config (--flip-h / --flip-v / --rotate).
                if (cfg_.flipH) layers[layerCount - 1].flipX = true;
                if (cfg_.flipV) layers[layerCount - 1].flipY = true;
                if (cfg_.rotate) layers[layerCount - 1].rotate = cfg_.rotate & 3;
                if (cfg_.chroma && cfg_.chromaLayer == 1) applyKey(layers[layerCount - 1]);
            } else if ((size_t)(ci - 1) < extraCaps_.size()) {
                ExtraCapture* e = extraCaps_[ci - 1].get();
                Microsoft::WRL::ComPtr<ID3D11Texture2D> frame;
                {
                    std::lock_guard<std::mutex> lk(e->mu);
                    frame = e->pending;
                    e->pending.Reset();
                }
                if (frame) {
                    D3D11_TEXTURE2D_DESC d{};
                    frame->GetDesc(&d);
                    if (!e->tex || e->w != d.Width || e->h != d.Height) {
                        e->w = d.Width;
                        e->h = d.Height;
                        D3D11_TEXTURE2D_DESC td = d;
                        td.Usage = D3D11_USAGE_DEFAULT;
                        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        td.CPUAccessFlags = 0;
                        td.MiscFlags = 0;
                        e->tex.Reset();
                        e->srv.Reset();
                        if (SUCCEEDED(
                                d3d_.device->CreateTexture2D(&td, nullptr, e->tex.GetAddressOf())))
                            e->srv = Compositor::CreateSrv(d3d_.device.Get(), e->tex.Get());
                    }
                    if (e->tex) d3d_.context->CopyResource(e->tex.Get(), frame.Get());
                }
                if (e->srv && e->w > 0) {
                    float x, y, w, h;
                    if (s.w > 0) {
                        x = (float)s.x;
                        y = (float)s.y;
                        w = (float)s.w;
                        h = (float)s.h;
                    } else {
                        const float margin = 16.f;
                        w = canvasW / 3.0f;
                        h = w * (float)e->h / (float)e->w;
                        const bool left = s.corner == 0 || s.corner == 2;
                        const bool top = s.corner == 0 || s.corner == 1;
                        x = left ? margin : canvasW - w - margin;
                        y = top ? margin : canvasH - h - margin;
                    }
                    pushLayer(e->srv.Get(), x, y, w, h);
                    applyXform(s);
                }
            }
        } else if (s.type == SessionSourceType::Webcam) {
            VideoLayer* v = FindVideo(s.device);
            if (!v || !v->cam) continue;
            std::vector<uint8_t> camBuf;
            uint32_t cw = 0, ch = 0;
            uint64_t cno = 0;
            if (v->cam->GetLatest(&camBuf, &cw, &ch, &cno)) {
                if (!v->tex || v->w != cw || v->h != ch) {
                    v->w = cw;
                    v->h = ch;
                    D3D11_TEXTURE2D_DESC wd = {};
                    wd.Width = cw;
                    wd.Height = ch;
                    wd.MipLevels = 1;
                    wd.ArraySize = 1;
                    wd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    wd.SampleDesc = {1, 0};
                    wd.Usage = D3D11_USAGE_DYNAMIC;
                    wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    wd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    v->tex.Reset();
                    if (SUCCEEDED(d3d_.device->CreateTexture2D(&wd, nullptr,
                                                               v->tex.GetAddressOf())))
                        v->srv = Compositor::CreateSrv(d3d_.device.Get(), v->tex.Get());
                }
                if (v->tex) {
                    D3D11_MAPPED_SUBRESOURCE map{};
                    if (SUCCEEDED(d3d_.context->Map(v->tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                                    &map))) {
                        const size_t rowBytes = (size_t)cw * 4;
                        for (uint32_t y = 0; y < ch; ++y)
                            memcpy((uint8_t*)map.pData + (size_t)y * map.RowPitch,
                                   camBuf.data() + (size_t)y * rowBytes, rowBytes);
                        d3d_.context->Unmap(v->tex.Get(), 0);
                    }
                }
            }
            if (v->srv && v->w > 0) {
                float x, y, w, h;
                const float defW =
                    cfg_.webcamWidth > 0 ? (float)cfg_.webcamWidth : canvasW / 4.0f;
                const float defH = defW * (float)v->h / (float)v->w;
                if (s.xr >= 0.f && s.yr >= 0.f) {
                    // Free position (dragged on the UI preview), clamped on screen.
                    w = defW;
                    h = defH;
                    x = s.xr * canvasW;
                    y = s.yr * canvasH;
                    if (x + w > canvasW) x = canvasW - w;
                    if (y + h > canvasH) y = canvasH - h;
                    if (x < 0) x = 0;
                    if (y < 0) y = 0;
                } else if (s.w > 0) {
                    x = (float)s.x;
                    y = (float)s.y;
                    w = (float)s.w;
                    h = (float)s.h;
                } else {
                    const float margin = 16.f;
                    const bool left = s.corner == 0 || s.corner == 2;
                    const bool top = s.corner == 0 || s.corner == 1;
                    w = defW;
                    h = defH;
                    x = left ? margin : canvasW - w - margin;
                    y = top ? margin : canvasH - h - margin;
                }
                pushLayer(v->srv.Get(), x, y, w, h);
                applyXform(s);
                if (cfg_.chroma && cfg_.chromaLayer == 0) applyKey(layers[layerCount - 1]);
            }
        } else if (s.type == SessionSourceType::Browser) {
            if (!browserOn_) continue;
            std::vector<uint8_t> frame;
            uint32_t bw = 0, bh = 0;
            uint64_t bno = 0;
            if (browser_.GetLatest(&frame, &bw, &bh, &bno)) {
                if (!browserTex_ || browserW_ != bw || browserH_ != bh) {
                    browserW_ = bw;
                    browserH_ = bh;
                    D3D11_TEXTURE2D_DESC bd = {};
                    bd.Width = bw;
                    bd.Height = bh;
                    bd.MipLevels = 1;
                    bd.ArraySize = 1;
                    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    bd.SampleDesc = {1, 0};
                    // Default usage + shared so the UI can draw the overlay without a
                    // 9 MB CPU upload per frame (the Settings overlay preview).
                    bd.Usage = D3D11_USAGE_DEFAULT;
                    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    bd.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                    ComPtr<ID3D11Texture2D> made;
                    if (SUCCEEDED(d3d_.device->CreateTexture2D(&bd, nullptr,
                                                               made.GetAddressOf()))) {
                        browserSrv_ = Compositor::CreateSrv(d3d_.device.Get(), made.Get());
                        std::lock_guard<std::mutex> lk(browserTexMu_);
                        browserTex_ = made;
                    }
                }
                ComPtr<ID3D11Texture2D> tex;
                {
                    std::lock_guard<std::mutex> lk(browserTexMu_);
                    tex = browserTex_;
                }
                if (tex) {
                    // UpdateSubresource stages internally: no per-row CPU mapping.
                    d3d_.context->UpdateSubresource(tex.Get(), 0, nullptr, frame.data(), bw * 4, 0);
                }
                {
                    std::lock_guard<std::mutex> lk(browserTexMu_);
                    browserFrameNo_ = bno;
                }
            }
            if (browserSrv_ && browserW_ > 0) {
                const float w = (float)(s.w > 0 ? s.w : (int)canvasW);
                const float h = (float)(s.h > 0 ? s.h : (int)canvasH);
                pushLayer(browserSrv_.Get(), (float)s.x, (float)s.y, w, h);
                applyXform(s);
                layers[layerCount - 1].useAlpha = true;
                layers[layerCount - 1].premultiplied = true;  // CEF OSR paints premultiplied BGRA
            }
        }
    }

    // ---- hardware cursor (DXGI duplication reports it separately) ----
    if (usingDxgi_.load() && showCursorLive_.load()) {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        BOOL gotInfo = GetCursorInfo(&ci);
        static bool cursorLogged = false;
        if (!cursorLogged) {
            cursorLogged = true;
            Log("[cursor] GetCursorInfo=%d flags=0x%X hcur=%p pos=%ld,%ld", (int)gotInfo,
                (unsigned)ci.flags, (void*)ci.hCursor, ci.ptScreenPos.x, ci.ptScreenPos.y);
        }
        if (gotInfo && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
            if (ci.hCursor != cursorHandle_) {
                bool fetched = FetchCursor(ci.hCursor);
                Log("[cursor] shape fetch=%d %ux%u hot=%d,%d", (int)fetched, cursorW_, cursorH_,
                    cursorHotX_, cursorHotY_);
                if (fetched) {
                    cursorHandle_ = ci.hCursor;
                    UploadCursorTexture();
                    Log("[cursor] texture=%s", cursorSrv_ ? "ok" : "FAILED");
                }
            }
            if (cursorSrv_) {
                const int cx = ci.ptScreenPos.x - (int)dup_.originX;
                const int cy = ci.ptScreenPos.y - (int)dup_.originY;
                layers[layerCount] = {cursorSrv_.Get(), (float)(cx - cursorHotX_),
                                      (float)(cy - cursorHotY_), (float)cursorW_,
                                      (float)cursorH_, 1.0f};
                layers[layerCount].useAlpha = true;
                layerCount++;
            }
        } else {
            cursorHandle_ = nullptr;  // cursor hidden: re-fetch when it returns
        }
    }

    if (fadeAlpha > 0.001f && transitionSrv_) {
        // Draw the outgoing frame on top of the new scene, fading out.
        if (layerCount < 8) {
            layers[layerCount] = {transitionSrv_.Get(), 0.f, 0.f, (float)canvasW,
                                  (float)canvasH, fadeAlpha};
            layerCount++;
        }
    }

    compositor_.Render(rtv_.Get(), layers, layerCount);

    // ---- preview (shared with the UI) ----
    // Copy every frame: this is a GPU-side copy, and skipping frames is what made
    // the on-screen preview run at a third of the capture rate.
    uint64_t seen = framesSeen_.fetch_add(1);
    if (previewTex_) {
        d3d_.context->CopyResource(previewTex_.Get(), tex_.Get());
        previewVersion_.fetch_add(1);
    }

    // ---- replay (its own continuous timeline) ----
    if (replay_.Enabled()) replay_.PushVideo(tex_.Get(), replayFrames_.fetch_add(1));
    else replayFrames_.fetch_add(1);

    if (paused_.load()) {
        skipped_.fetch_add(1);
        return;
    }

    bool anyOutput = false;

    if (recOn_.load()) {
        std::vector<uint8_t> data;
        bool key = false;
        int64_t outPts = -1;
        std::string e2;
        bool wantIdr = forceIdrRec_.exchange(false);
        const uint64_t idx = (uint64_t)((int64_t)vidOut_.load() - recBaseFrames_);
        if (recEnc_.Encode(tex_.Get(), idx, wantIdr, &data, &key, &outPts, &e2)) {
            if (!data.empty()) {
                std::string e3;
                if (!recMux_.WriteVideo(data.data(), data.size(), key, outPts, &e3)) {
                    Fail("record write failed: " + e3);
                    return;
                }
                anyOutput = true;
            }
        } else {
            failedFrames_.fetch_add(1);
        }
    }

    if (strOn_.load() && !recon_.load()) {
        std::vector<uint8_t> data;
        bool key = false;
        int64_t outPts = -1;
        std::string e2;
        bool wantIdr = forceIdrStr_.exchange(false);
        const uint64_t idx = (uint64_t)((int64_t)vidOut_.load() - strBaseFrames_);
        if (strEnc_.Encode(tex_.Get(), idx, wantIdr, &data, &key, &outPts, &e2)) {
            if (!data.empty()) {
                std::string e3;
                if (!strMux_.WriteVideo(data.data(), data.size(), key, outPts, &e3)) {
                    if (failures_.fetch_add(1) >= 1) recon_.store(true);
                } else {
                    anyOutput = true;
                }
            }
        } else {
            failedFrames_.fetch_add(1);
        }
    }

    if (anyOutput) encoded_.fetch_add(1);
    vidOut_.fetch_add(1);
}

bool Session::OnAudio(int track, const uint8_t* data, size_t size, int64_t pts) {
    lastAudioPts_.store(pts);
    if (replay_.Enabled() && track == 0) replay_.PushAudio(data, size, pts);
    if (recOn_.load() && recMux_.IsOpen()) {
        std::string e;
        recMux_.WriteAudio(track, data, size, pts - recBaseAudio_, &e);
    }
    if (strOn_.load() && !recon_.load() && strMux_.IsOpen()) {
        std::string e;
        strMux_.WriteAudio(0, data, size, pts - strBaseAudio_, &e);
    }
    return true;
}

bool Session::FetchCursor(HCURSOR hcur) {
    ICONINFO ii{};
    if (!GetIconInfo(hcur, &ii)) return false;

    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
    bool ok = false;
    HDC dc = GetDC(nullptr);

    if (ii.hbmColor && dc) {
        BITMAP bm{};
        if (GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
            w = bm.bmWidth;
            h = bm.bmHeight;
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;  // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            bgra.assign((size_t)w * h * 4, 0);
            if (GetDIBits(dc, ii.hbmColor, 0, (UINT)h, bgra.data(), &bi, DIB_RGB_COLORS) == h) {
                ok = true;
                bool anyAlpha = false;
                for (size_t i = 3; i < bgra.size(); i += 4)
                    if (bgra[i]) {
                        anyAlpha = true;
                        break;
                    }
                if (!anyAlpha) {
                    // Legacy cursor without an alpha channel: start opaque and
                    // clear only the pixels the AND mask marks as transparent.
                    for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
                    if (ii.hbmMask) {
                        const size_t stride = ((size_t)(w + 31) / 32) * 4;
                        std::vector<uint8_t> mask(stride * (size_t)h, 0);
                        BITMAPINFO bi1{};
                        bi1.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bi1.bmiHeader.biWidth = w;
                        bi1.bmiHeader.biHeight = -h;
                        bi1.bmiHeader.biPlanes = 1;
                        bi1.bmiHeader.biBitCount = 1;
                        bi1.bmiHeader.biCompression = BI_RGB;
                        if (GetDIBits(dc, ii.hbmMask, 0, (UINT)h, mask.data(), &bi1,
                                      DIB_RGB_COLORS) == h) {
                            for (int y = 0; y < h; ++y)
                                for (int x = 0; x < w; ++x) {
                                    const uint8_t bit = (uint8_t)(0x80 >> (x & 7));
                                    if (mask[(size_t)y * stride + x / 8] & bit)
                                        bgra[((size_t)y * w + x) * 4 + 3] = 0;
                                }
                        }
                    }
                }
            }
        }
    }

    if (!ok && ii.hbmMask && dc) {
        // Monochrome cursor: AND mask rows followed by XOR mask rows, 1bpp.
        BITMAP bm{};
        if (GetObjectW(ii.hbmMask, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight >= 2) {
            w = bm.bmWidth;
            h = bm.bmHeight / 2;
            const size_t stride = ((size_t)(w + 31) / 32) * 4;
            std::vector<uint8_t> mask(stride * (size_t)bm.bmHeight, 0);
            BITMAPINFO bi1{};
            bi1.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi1.bmiHeader.biWidth = w;
            bi1.bmiHeader.biHeight = -bm.bmHeight;
            bi1.bmiHeader.biPlanes = 1;
            bi1.bmiHeader.biBitCount = 1;
            bi1.bmiHeader.biCompression = BI_RGB;
            if (GetDIBits(dc, ii.hbmMask, 0, (UINT)bm.bmHeight, mask.data(), &bi1,
                          DIB_RGB_COLORS) == bm.bmHeight) {
                bgra.assign((size_t)w * h * 4, 0);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const uint8_t bit = (uint8_t)(0x80 >> (x & 7));
                        const bool a = (mask[(size_t)y * stride + x / 8] & bit) != 0;
                        const bool xo = (mask[(size_t)(h + y) * stride + x / 8] & bit) != 0;
                        uint8_t* px = bgra.data() + ((size_t)y * w + x) * 4;
                        if (xo) {
                            px[0] = px[1] = px[2] = 255;
                            px[3] = 255;
                        } else if (a) {
                            px[3] = 0;
                        } else {
                            px[0] = px[1] = px[2] = 0;
                            px[3] = 255;
                        }
                    }
                ok = true;
            }
        }
    }

    if (dc) ReleaseDC(nullptr, dc);
    const int hotX = (int)ii.xHotspot, hotY = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (!ok) return false;

    cursorW_ = (uint32_t)w;
    cursorH_ = (uint32_t)h;
    cursorHotX_ = hotX;
    cursorHotY_ = hotY;
    cursorBgra_ = std::move(bgra);
    return true;
}

bool Session::UploadCursorTexture() {
    if (cursorBgra_.empty() || cursorW_ == 0 || cursorH_ == 0) return false;

    D3D11_TEXTURE2D_DESC cd = {};
    cd.Width = cursorW_;
    cd.Height = cursorH_;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    cd.SampleDesc = {1, 0};
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cursorTex_.Reset();
    cursorSrv_.Reset();
    if (FAILED(d3d_.device->CreateTexture2D(&cd, nullptr, cursorTex_.GetAddressOf()))) return false;
    cursorSrv_ = Compositor::CreateSrv(d3d_.device.Get(), cursorTex_.Get());
    if (!cursorSrv_) return false;

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(d3d_.context->Map(cursorTex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return false;
    const size_t rowBytes = (size_t)cursorW_ * 4;
    for (uint32_t y = 0; y < cursorH_; ++y)
        memcpy((uint8_t*)map.pData + (size_t)y * map.RowPitch,
               cursorBgra_.data() + (size_t)y * rowBytes, rowBytes);
    d3d_.context->Unmap(cursorTex_.Get(), 0);
    return true;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> Session::PreviewTexture() const {
    std::lock_guard<std::mutex> lock(previewMu_);
    return previewTex_;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> Session::BrowserTexture(uint64_t* frameNo) const {
    std::lock_guard<std::mutex> lock(browserTexMu_);
    if (frameNo) *frameNo = browserFrameNo_;
    return browserTex_;
}

Session::Stats Session::GetStats() const {    Stats st;
    st.running = running_.load() && !stop_.load();
    st.paused = paused_.load();
    st.reconnecting = recon_.load();
    st.failed = failed_.load();
    {
        std::lock_guard<std::mutex> lock(errMu_);
        st.error = failErr_;
    }
    st.elapsed = ElapsedSeconds();
    {
        const auto now = std::chrono::steady_clock::now();
        if (recOn_.load() && recStart_ != std::chrono::steady_clock::time_point{})
            st.recSeconds = std::chrono::duration<double>(now - recStart_).count();
        if (strOn_.load() && strStart_ != std::chrono::steady_clock::time_point{})
            st.strSeconds = std::chrono::duration<double>(now - strStart_).count();
    }
    st.captured = pacedFrames_.load();
    st.encoded = encoded_.load();
    st.failedFrames = failedFrames_.load();
    st.dropped = failedFrames_.load();
    st.skippedPaused = skipped_.load();
    st.kbps = kbps_.load();
    st.clipsSaved = replay_.SaveCount();
    st.backend = usingDxgi_.load() ? "DXGI" : "WGC";
    st.width = width_.load();
    st.height = height_.load();
    st.dxgiPointer = dup_.pointerUpdates.load();
    st.dxgiShapes = dup_.shapeUpdates.load();
    st.desktopDevice = mixer_.DesktopName();
    st.micDevice = mixer_.MicName();
    st.webcamName.clear();
    for (auto& v : videos_) {
        if (!v.cam) continue;
        if (!st.webcamName.empty()) st.webcamName += L" + ";
        st.webcamName += v.cam->ActiveDevice();
    }
    st.browserActive = browserOn_ && browserW_ > 0;
    st.recording = recOn_.load();
    st.streaming = strOn_.load();
    st.replayArmed = replay_.Enabled();
    return st;
}
