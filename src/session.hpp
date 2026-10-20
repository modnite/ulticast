#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

#include "core/audio_mixer.hpp"
#include "core/browser_cef.hpp"
#include "core/capture_dshow.hpp"
#include "core/capture_dxgi.hpp"
#include "core/capture_wgc.hpp"
#include "core/compositor.hpp"
#include "core/d3d.hpp"
#include "core/encoder_config.hpp"
#include "core/ffmuxer.hpp"
#include "core/nvenc_encoder.hpp"
#include "core/replay_buffer.hpp"
#include "core/webcam_mf.hpp"

// A source placed in the scene, in canvas pixels. w/h == 0 means "use the
// source's default size" (full canvas for capture/browser, PiP for video).
enum class SessionSourceType { Capture, Webcam, Browser, AudioIn, AudioOut, Window };

struct SessionSource {
    SessionSourceType type = SessionSourceType::Capture;
    bool enabled = true;
    int x = 0, y = 0, w = 0, h = 0;
    // Optional fractional position (0..1 of the canvas) for video PiPs;
    // -1 = use the corner preset. Used by drag-to-move on the UI preview.
    float xr = -1.f, yr = -1.f;
    // Optional fractional size (0..1 of the canvas); -1 = the source's default.
    // Set by drag-to-resize on the UI preview.
    float wr = -1.f, hr = -1.f;
    // Expanded settings (all optional; empty/0 = defaults):
    std::string device;        // Video: capture-device name substring ("" = default)
    int monitor = 0;           // Capture: monitor index for WGC/DXGI
    std::wstring window;       // Capture: window title substring (non-empty = window capture)
    bool dxgi = false;         // Capture: use DXGI duplication (fullscreen games)
    int corner = 3;            // Video: PiP corner (0 TL, 1 TR, 2 BL, 3 BR)
    std::wstring audioDevice;  // AudioIn/Out: device name substring ("" = default)
    // Orientation: flip and quarter-turn rotation, applied on the GPU when this
    // source's layer is composited.
    bool flipH = false, flipV = false;
    int rotate = 0;            // 0..3 quarter turns
};

// Everything a run needs. The UI fills this from its widgets; the CLI from flags.
struct SessionConfig {
    // capture
    int monitor = 0;
    std::wstring window;            // non-empty = window capture
    bool windowSource = false;      // the source is a window capture (window must be set)
    std::string backend = "auto";   // auto | wgc | dxgi
    uint32_t fps = 60;
    int adapter = -1;

    // outputs (both can be enabled)
    bool record = true;
    std::string recordPath = "record.mkv";
    bool stream = false;
    std::string streamUrl;
    bool streamHevc = false;
    uint32_t streamBitrate = 6000;
    int reconnect = 5;

    // recording encoder
    bool hevc = true;
    uint32_t cqp = 20;              // CQ level 1..51
    bool recordCbr = false;         // record with CBR bitrate instead of CQ
    uint32_t recordBitrate = 12000; // kbps when recordCbr
    int preset = 5;
    int bframes = 0;
    bool recordLookahead = false;
    bool recordMultipass = false;

    // stream encoder (separate settings so each output is tuned on its own)
    int streamPreset = 5;
    int streamBframes = 0;
    bool streamLookahead = false;
    bool streamMultipass = false;

    // audio
    bool noAudio = false;
    AudioConfig audio;

    // webcam overlay
    bool webcam = false;
    std::wstring webcamDevice;
    std::string webcamPos = "bottomright";
    int webcamWidth = 0;

    // replay buffer
    bool replay = false;
    int replaySeconds = 30;
    std::string replayPrefix = "Replay";

    // chroma key (GPU-side, per layer)
    bool chroma = false;
    std::string chromaColor = "00FF00";
    int chromaLayer = 0;  // 0 = webcam overlay, 1 = capture

    // browser overlay (CEF offscreen rendering)
    bool browser = false;
    std::string browserUrl;      // http(s) URL or local HTML file
    int browserWidth = 0;        // 0 = canvas size
    int browserHeight = 0;

    bool showCursor = true;      // DXGI duplication cursor compositing
    // Orientation (flip / quarter turns), applied when the layer is composited.
    bool flipH = false, flipV = false;
    int rotate = 0;
    bool forceDshow = false;     // debug/testing: try DirectShow before Media Foundation
    bool sceneFade = true;       // crossfade ~250 ms when the scene changes

    // Scene: ordered sources. Empty -> a default scene is built from the
    // webcam/browser flags above (keeps the CLI behaviour).
    std::vector<SessionSource> sources;
};

// One run of the capture pipeline: capture -> compositor -> encoders -> outputs.
// Self-contained: owns its D3D device and a supervisor thread (reconnects,
// DXGI fallback, throughput accounting). Safe to Start/Stop repeatedly.
class Session {
public:
    struct Stats {
        bool running = false;
        bool paused = false;
        bool reconnecting = false;
        bool failed = false;
        std::string error;
        double elapsed = 0;
        double recSeconds = 0;   // how long the current recording has run
        double strSeconds = 0;   // how long the current stream has run
        uint64_t captured = 0;
        uint64_t encoded = 0;
        uint64_t failedFrames = 0;
        uint64_t dropped = 0;       // frames the encoder refused (visual drop indicator)
        uint64_t skippedPaused = 0;
        bool recording = false;     // live output state (runtime switchable)
        bool streaming = false;
        bool replayArmed = false;
        double kbps = 0;
        int clipsSaved = 0;
        std::string backend;
        uint32_t width = 0, height = 0;
        uint64_t dxgiPointer = 0, dxgiShapes = 0;
        std::wstring desktopDevice, micDevice, webcamName;
        // Post-fader peaks for the mixer meters (0..1 linear).
        float deskPeak = 0.f, micPeak = 0.f;
        bool browserActive = false;
    };

    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool Start(const SessionConfig& cfg, std::string* err);
    void Stop();
    void SetPaused(bool paused);
    bool Paused() const { return paused_.load(); }
    bool SaveClip();  // false if replay disabled or a save is already running
    Stats GetStats() const;

    // Live scene updates, safe to call while running (UI scene switching).
    // webcamPos applies to video sources that use the default PiP placement.
    void SetSources(std::vector<SessionSource> sources, std::string webcamPos);
    // Show/hide the cursor: DXGI composites it ourselves, WGC draws it into the
    // frame, so both paths are toggled here.
    void SetCursorVisible(bool on);
    // Force the browser overlay to reload (no-op when it is not running).
    void ReloadBrowser() { browser_.Reload(); }
    // Point the running overlay at a new URL without restarting the session.
    void SetBrowserUrl(const std::string& url) { browser_.LoadUrl(url); }
    // Latest overlay frame (packed BGRA) for the Settings overlay preview.
    bool BrowserFrame(std::vector<uint8_t>* out, uint32_t* w, uint32_t* h, uint64_t* ver) {
        return browserOn_ && browser_.GetLatest(out, w, h, ver);
    }
    // Where each scene layer was last composited, in canvas pixels: the UI uses
    // this for selection, drag-to-move and drag-to-resize on the preview.
    struct LayerRect {
        int index = -1;  // source index within the scene
        float x = 0, y = 0, w = 0, h = 0;
    };
    std::vector<LayerRect> LayerRects() const {
        std::lock_guard<std::mutex> lk(layerRectMu_);
        return layerRects_;
    }
    // The overlay's GPU texture (shared) + frame counter: the UI draws it
    // zero-copy, instead of uploading a 9 MB CPU buffer per frame.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> BrowserTexture(uint64_t* frameNo) const;
    // Mute/unmute the audio sources at runtime (scene audio sources).
    void SetAudioSourcesLive(bool desktop, bool mic) { mixer_.SetLiveSources(desktop, mic); }
    // Mic noise gate (threshold in dBFS); safe to call while running.
    void SetAudioGate(bool on, float db) { mixer_.SetGate(on, db); }
    // Mic compressor (threshold dBFS, ratio); safe to call while running.
    void SetAudioCompressor(bool on, float db, float ratio) {
        mixer_.SetCompressor(on, db, ratio);
    }
    // Mic monitoring (hear the post-filter mic in headphones).
    void SetAudioMonitoring(bool on) { mixer_.SetMonitoring(on, L"default"); }

    // Runtime output control (OBS-style separate buttons). Commands are queued
    // and applied on the capture thread between frames, so they never race with
    // OnFrame. Failures are logged; Stats reports the live state.
    void EnableRecording(const std::string& path);
    void DisableRecording();
    void EnableStreaming(const std::string& url, uint32_t bitrateKbps);
    void DisableStreaming();
    void EnableReplay(int seconds, const std::string& prefix);
    void DisableReplay();

    // Optional sink for engine messages (UI activity strip). Thread-safe usage
    // is the caller's responsibility; the UI wraps it in a mutex.
    using LogFn = std::function<void(const std::string&)>;
    void SetLogSink(LogFn fn) { logFn_ = std::move(fn); }

    // UI preview: a shared texture the UI can open on its own D3D device.
    // Returns a reference-counted copy (safe across threads); null until the
    // first frame arrives. Version bumps as it is refreshed.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> PreviewTexture() const;
    uint64_t PreviewVersion() const { return previewVersion_.load(); }

private:
    void OnFrame(ID3D11Texture2D* src, uint32_t w, uint32_t h, uint64_t idx);
    bool OnAudio(int track, const uint8_t* data, size_t size, int64_t pts);
    bool InitPipeline(uint32_t w, uint32_t h, std::string* err);
    void Supervisor();
    bool DoReconnect();
    void Fail(const std::string& msg);
    void Log(const char* fmt, ...);
    double ElapsedSeconds() const;

    // Runtime output commands (applied on the capture thread).
    enum class OutCmdType { RecOn, RecOff, StrOn, StrOff, RepOn, RepOff };
    struct OutCmd {
        OutCmdType type = OutCmdType::RecOn;
        std::string arg;      // path / url / prefix
        uint32_t bitrate = 0;
        int seconds = 30;
    };
    void ApplyOutputCommands();
    bool BeginRecording(const std::string& path);
    void EndRecording();
    bool BeginStreaming(const std::string& url, uint32_t bitrateKbps);
    void EndStreaming();
    bool BeginReplay(int seconds, const std::string& prefix);
    void EndReplay();
    EncoderConfig MakeEncoderConfig(uint32_t w, uint32_t h) const;

    LogFn logFn_;

    SessionConfig cfg_{};
    D3DContext d3d_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> recon_{false};
    std::atomic<int> failures_{0};
    std::atomic<bool> forceIdrRec_{false};
    std::atomic<bool> forceIdrStr_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> inited_{false};
    std::atomic<bool> usingDxgi_{false};
    std::atomic<bool> dxgiChecked_{false};
    std::atomic<uint32_t> width_{0}, height_{0};
    std::atomic<uint64_t> encoded_{0}, failedFrames_{0}, skipped_{0};
    std::atomic<uint64_t> pacedFrames_{0};
    std::atomic<uint64_t> vidOut_{0}, replayFrames_{0}, framesSeen_{0};
    std::atomic<uint64_t> previewVersion_{0};
    std::atomic<double> kbps_{0.0};
    std::atomic<double> lastElapsed_{0.0};
    std::chrono::steady_clock::time_point t0_{};
    std::chrono::steady_clock::time_point lastFrameTime_{};
    std::chrono::steady_clock::time_point recStart_{}, strStart_{};

    // Scene crossfade: SetSources() asks for a snapshot of the outgoing frame
    // (the D3D context is capture-thread only) and OnFrame blends it out.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> transitionTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> transitionSrv_;
    std::atomic<bool> transitionPending_{false};
    std::chrono::steady_clock::time_point transitionStart_{};
    std::thread supervisor_;
    mutable std::mutex previewMu_;
    mutable std::mutex browserTexMu_;  // guards the overlay texture handoff to the UI
    mutable std::mutex layerRectMu_;
    std::vector<LayerRect> layerRects_;  // last composited layer rects (canvas px)

    mutable std::mutex errMu_;
    mutable std::string failErr_;
    WgcCapture cap_;
    DxgiDuplicator dup_;
    NvencEncoder recEnc_, strEnc_;
    FfMuxer recMux_, strMux_;
    FfMuxer::Config recCfg_, strCfg_;
    Compositor compositor_;
    ReplayBuffer replay_;
    BrowserOverlay browser_;
    AudioMixer mixer_;

    // One entry per Video source (webcam, capture card, ...). Multiple devices
    // can be composited at once; each owns its capture thread and texture.
    struct VideoLayer {
        std::unique_ptr<VideoCaptureBase> cam;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t w = 0, h = 0;
        std::string device;
    };
    std::vector<VideoLayer> videos_;
    VideoLayer* FindVideo(const std::string& device);
    void StartVideoSources();

    // Secondary capture sources (a second monitor/window as its own layer).
    // Each owns a WGC capture; its callback only parks the newest frame (the
    // D3D immediate context is not thread-safe) and the main tick composites it.
    struct ExtraCapture {
        std::unique_ptr<WgcCapture> cap;
        std::mutex mu;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> pending;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t w = 0, h = 0;
        int monitor = 0;
        std::wstring window;
    };
    std::vector<std::unique_ptr<ExtraCapture>> extraCaps_;
    void StartExtraCaptures();
    void StopExtraCaptures();

    bool audioReady_ = false;
    bool webcamOn_ = false;
    bool browserOn_ = false;
    std::atomic<bool> webcamActive_{false};
    float keyR_ = 0.f, keyG_ = 1.f, keyB_ = 0.f;

    // Live scene state (guarded by sourcesMu_); OnFrame snapshots it per frame.
    mutable std::mutex sourcesMu_;
    std::vector<SessionSource> liveSources_;
    std::string liveWebcamPos_ = "bottomright";
    std::atomic<bool> showCursorLive_{true};

    // Runtime outputs (capture thread owns the bases; UI thread only enqueues).
    std::mutex cmdMu_;
    std::deque<OutCmd> cmds_;
    std::atomic<bool> recOn_{false}, strOn_{false};
    int64_t recBaseFrames_ = 0, strBaseFrames_ = 0;
    int64_t recBaseAudio_ = 0, strBaseAudio_ = 0;
    std::atomic<int64_t> lastAudioPts_{0};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneTex_;
    // Set when the captured frame's size differs from the canvas (odd window
    // size or a window that was resized): the frame is drawn scaled.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scaledFrameSrv_;
    bool sizeScaled_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> previewTex_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> browserTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> browserSrv_;
    std::vector<uint8_t> browserBuf_;
    uint64_t browserFrameNo_ = 0;
    uint32_t browserW_ = 0, browserH_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> cursorTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursorSrv_;
    uint32_t cursorW_ = 0, cursorH_ = 0;
    uint64_t cursorVersion_ = 0;
    HCURSOR cursorHandle_ = nullptr;
    std::vector<uint8_t> cursorBgra_;
    int cursorHotX_ = 0, cursorHotY_ = 0;
    bool FetchCursor(HCURSOR hcur);
    bool UploadCursorTexture();
};
