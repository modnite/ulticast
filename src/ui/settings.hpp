#pragma once
#include <string>
#include <vector>
#include <windows.h>

// One scene: a named source layout the user can switch between.
struct UiSceneSetting {
    std::string name = "Scene";
    int capture = 1;
    int webcam = 0;
    int browser = 0;
    int cursor = 1;
    int corner = 3;  // webcam position preset index
    float camX = -1;  // free webcam position (0..1 of canvas); -1 = use corner
    float camY = -1;
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

    bool webcam = false;
    std::string webcamDevice;  // UTF-8 substring; empty = default camera
    std::string webcamPos = "bottomright";

    bool record = true;
    std::string recordPath;

    bool stream = false;
    uint32_t streamBitrate = 6000;  // kbps (CBR)
    std::string platform = "twitch";
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

    int hotkey = VK_F9;
};

bool LoadUiSettings(UiSettings* s);
bool SaveUiSettings(const UiSettings& s);
std::string DefaultRecordingPath();
