#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

#include "core/audio_mixer.hpp"
#include "core/browser_cef.hpp"
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
// source's default size" (full canvas for capture/browser, PiP for webcam).
enum class SessionSourceType { Capture, Webcam, Browser };

struct SessionSource {
    SessionSourceType type = SessionSourceType::Capture;
    bool enabled = true;
    int x = 0, y = 0, w = 0, h = 0;
};

// Everything a run needs. The UI fills this from its widgets; the CLI from flags.
struct SessionConfig {
    // capture
    int monitor = 0;
    std::wstring window;            // non-empty = window capture
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
    uint32_t cqp = 20;
    int preset = 5;

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
        uint64_t captured = 0;
        uint64_t encoded = 0;
        uint64_t failedFrames = 0;
        uint64_t skippedPaused = 0;
        double kbps = 0;
        int clipsSaved = 0;
        std::string backend;
        uint32_t width = 0, height = 0;
        uint64_t dxgiPointer = 0, dxgiShapes = 0;
        std::wstring desktopDevice, micDevice, webcamName;
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
    // webcamPos applies to webcam sources that use the default PiP placement.
    void SetSources(std::vector<SessionSource> sources, std::string webcamPos);
    void SetCursorVisible(bool on) { showCursorLive_.store(on); }

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
    std::thread supervisor_;
    mutable std::mutex previewMu_;

    mutable std::mutex errMu_;
    mutable std::string failErr_;
    WgcCapture cap_;
    DxgiDuplicator dup_;
    NvencEncoder recEnc_, strEnc_;
    FfMuxer recMux_, strMux_;
    FfMuxer::Config recCfg_, strCfg_;
    Compositor compositor_;
    ReplayBuffer replay_;
    WebcamCapture webcam_;
    BrowserOverlay browser_;
    AudioMixer mixer_;

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

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSrv_, webcamSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneTex_, webcamTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> previewTex_;
    uint32_t webcamW_ = 0, webcamH_ = 0;
    float camX_ = 0, camY_ = 0, camW_ = 0, camH_ = 0;

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
