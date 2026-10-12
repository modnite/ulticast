#pragma once
#include <string>
#include <windows.h>

// Persisted UI settings. The stream key is stored DPAPI-encrypted (per user).
struct UiSettings {
    int monitor = 0;
    std::string windowTitle;  // UTF-8; empty = full monitor capture
    std::string backend = "auto";
    uint32_t fps = 60;

    bool hevc = true;
    uint32_t cqp = 20;
    int preset = 5;

    bool desktopAudio = true;
    bool mic = false;
    std::string micDevice;      // UTF-8 substring; empty = default device
    std::string desktopDevice;  // UTF-8 substring; empty = default device

    bool webcam = false;
    std::string webcamDevice;  // UTF-8 substring; empty = default camera
    std::string webcamPos = "bottomright";

    bool record = true;
    std::string recordPath;

    bool stream = false;
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

    int hotkey = VK_F9;
};

bool LoadUiSettings(UiSettings* s);
bool SaveUiSettings(const UiSettings& s);
std::string DefaultRecordingPath();
