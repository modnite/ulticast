#pragma once
#include <string>
#include <vector>
#include <windows.h>

// One source inside a scene.
struct UiSourceSetting {
    int type = 0;          // 0 Capture, 1 Video device, 2 Browser, 3 Audio in, 4 Audio out
    int enabled = 1;
    std::string device;    // Video/Audio: device name ("" = default)
    int monitor = 0;       // Capture
    std::string window;    // Capture: window title substring ("" = monitor)
    int dxgi = 0;          // Capture: DXGI duplication
    int corner = 3;        // Video: PiP corner
    float camX = -1, camY = -1;  // Video: free PiP position
};

// One scene: a named source layout the user can switch between.
struct UiSceneSetting {
    std::string name = "Scene";
    int cursor = 1;
    std::vector<UiSourceSetting> sources;
};

// Persisted UI settings. The stream key is stored DPAPI-encrypted (per user).
struct UiSettings {
    int monitor = 0;
    std::string windowTitle;  // UTF-8; empty = full monitor capture
    std::string backend = "wgc";
    int gpu = -1;  // DXGI adapter index; -1 = auto (NVIDIA GPU preferred)
    uint32_t fps = 60;

    bool hevc = true;
    uint32_t cqp = 20;            // CQ level 1..51
    bool recordCbr = false;       // record with CBR bitrate instead of CQ
    uint32_t recordBitrate = 12000;
    int bframes = 0;              // 0..4
    bool lookahead = false;
    bool multipass = false;
    int preset = 5;

    bool desktopAudio = true;
    bool mic = false;
    std::string micDevice;      // UTF-8 substring; empty = default device
    std::string desktopDevice;  // UTF-8 substring; empty = default device
    float micGain = 1.0f;
    float desktopGain = 1.0f;
    bool micGate = false;   // mic noise gate
    int micGateDb = -45;    // gate threshold in dBFS
    bool micComp = false;   // mic compressor
    int micCompDb = -18;    // compressor threshold in dBFS
    int micCompRatio = 30;  // ratio x10 (30 = 3.0:1)
    bool monitorMic = false;  // hear the mic in the default output device

    bool webcam = false;
    std::string webcamDevice;  // UTF-8 substring; empty = default camera
    std::string webcamPos = "bottomright";

    bool record = true;
    std::string recordPath;

    bool stream = false;
    uint32_t streamBitrate = 6000;  // kbps (CBR)
    bool streamHevc = false;        // H.264 by default (Twitch etc.)
    int streamReconnect = 5;        // reconnect attempts
    std::string platform = "twitch";
    std::string twitchClientId;      // empty = built-in app id
    std::string twitchRefreshToken;  // DPAPI-encrypted in the ini
    std::string twitchChannel;       // linked channel display name
    std::string streamKey;
    std::string customUrl;

    bool replay = false;
    int replaySeconds = 30;
    std::string replayPrefix = "Replay";

    bool chroma = false;
    std::string chromaColor = "00FF00";

    bool browser = false;
    std::string browserUrl;

    // Scenes: source layouts the user switches between. Always at least one.
    std::vector<UiSceneSetting> scenes;
    int activeScene = 0;

    bool idlePreview = true;    // capture-only preview while not recording
    bool closeToTray = false;   // X keeps running in the tray instead of exiting
    int accent = 0;             // index into the accent table (Violet default)
    bool light = false;         // light theme instead of dark

    int hotkey = VK_F9;
};

bool LoadUiSettings(UiSettings* s);
bool SaveUiSettings(const UiSettings& s);
std::string DefaultRecordingPath();
