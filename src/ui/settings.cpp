#include "settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <shlobj.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

static std::string SettingsPath() {
    const char* appdata = getenv("APPDATA");
    std::string dir = appdata ? appdata : ".";
    dir += "\\Ulticast";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.ini";
}

// ---- DPAPI helpers ---------------------------------------------------------

static std::string Base64Encode(const BYTE* data, DWORD size) {
    DWORD len = 0;
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    if (!len) return {};
    std::string out(len, '\0');
    CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &len);
    out.resize(strnlen_s(out.c_str(), out.size()));
    return out;
}

static bool Base64Decode(const std::string& text, std::vector<BYTE>* out) {
    DWORD size = 0;
    if (!CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr,
                              nullptr))
        return false;
    out->resize(size);
    if (!CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, out->data(), &size, nullptr,
                              nullptr))
        return false;
    out->resize(size);
    return true;
}

static std::string ProtectKey(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Ulticast stream key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string b64 = Base64Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return b64;
}

static std::string UnprotectKey(const std::string& b64) {
    if (b64.empty()) return {};
    std::vector<BYTE> blob;
    if (!Base64Decode(b64, &blob)) return {};
    DATA_BLOB in{(DWORD)blob.size(), blob.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out))
        return {};
    std::string plain((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

// ---- defaults --------------------------------------------------------------

std::string DefaultRecordingPath() {
    PWSTR videos = nullptr;
    std::string path = "record.mkv";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) {
        std::wstring w(videos);
        CoTaskMemFree(videos);
        std::string dir(w.begin(), w.end());
        dir += "\\Ulticast";
        CreateDirectoryA(dir.c_str(), nullptr);
        path = dir + "\\Recording.mkv";
    }
    return path;
}

// ---- INI -------------------------------------------------------------------

static void Trim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos)
        s.clear();
    else
        s.erase(0, start);
}

bool LoadUiSettings(UiSettings* s) {
    if (s->recordPath.empty()) s->recordPath = DefaultRecordingPath();

    std::ifstream f(SettingsPath());
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        Trim(line);
        if (line.empty() || line[0] == '[' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        Trim(key);
        Trim(val);

        if (key == "monitor") s->monitor = atoi(val.c_str());
        else if (key == "window") s->windowTitle = val;
        else if (key == "backend") s->backend = val;
        else if (key == "fps") s->fps = (uint32_t)atoi(val.c_str());
        else if (key == "hevc") s->hevc = (val == "1");
        else if (key == "cqp") s->cqp = (uint32_t)atoi(val.c_str());
        else if (key == "preset") s->preset = atoi(val.c_str());
        else if (key == "desktopAudio") s->desktopAudio = (val == "1");
        else if (key == "mic") s->mic = (val == "1");
        else if (key == "micDevice") s->micDevice = val;
        else if (key == "desktopDevice") s->desktopDevice = val;
        else if (key == "webcam") s->webcam = (val == "1");
        else if (key == "webcamDevice") s->webcamDevice = val;
        else if (key == "webcamPos") s->webcamPos = val;
        else if (key == "record") s->record = (val == "1");
        else if (key == "recordPath") s->recordPath = val;
        else if (key == "stream") s->stream = (val == "1");
        else if (key == "platform") s->platform = val;
        else if (key == "streamKeyEnc") s->streamKey = UnprotectKey(val);
        else if (key == "customUrl") s->customUrl = val;
        else if (key == "replay") s->replay = (val == "1");
        else if (key == "replaySeconds") s->replaySeconds = atoi(val.c_str());
        else if (key == "replayPrefix") s->replayPrefix = val;
        else if (key == "chroma") s->chroma = (val == "1");
        else if (key == "chromaColor") s->chromaColor = val;
        else if (key == "browser") s->browser = (val == "1");
        else if (key == "browserUrl") s->browserUrl = val;
        else if (key == "hotkey") s->hotkey = atoi(val.c_str());
        else if (key == "activeScene") s->activeScene = atoi(val.c_str());
        else if (key.rfind("scene", 0) == 0 && key.size() > 5 &&
                 isdigit((unsigned char)key[5])) {
            const int idx = atoi(key.c_str() + 5);
            if (idx >= 0 && idx < 16) {
                // name|capture|webcam|browser|cursor|corner
                std::vector<std::string> parts;
                size_t start = 0;
                while (true) {
                    size_t bar = val.find('|', start);
                    if (bar == std::string::npos) {
                        parts.push_back(val.substr(start));
                        break;
                    }
                    parts.push_back(val.substr(start, bar - start));
                    start = bar + 1;
                }
                while (parts.size() < 6) parts.push_back("0");
                UiSceneSetting sc;
                sc.name = parts[0].empty() ? "Scene" : parts[0];
                sc.capture = parts[1] == "0" ? 0 : 1;
                sc.webcam = parts[2] == "1" ? 1 : 0;
                sc.browser = parts[3] == "1" ? 1 : 0;
                sc.cursor = parts[4] == "0" ? 0 : 1;
                sc.corner = atoi(parts[5].c_str());
                if (sc.corner < 0 || sc.corner > 3) sc.corner = 3;
                if ((int)s->scenes.size() <= idx) s->scenes.resize(idx + 1);
                s->scenes[idx] = sc;
            }
        }
    }

    if (s->scenes.empty()) {
        // First run (or an upgrade from the single-scene builds): seed one scene,
        // carrying over the old global webcam/browser toggles.
        UiSceneSetting sc;
        sc.name = "Game";
        sc.webcam = s->webcam ? 1 : 0;
        sc.browser = s->browser ? 1 : 0;
        s->scenes.push_back(sc);
    }
    return true;
}

bool SaveUiSettings(const UiSettings& s) {
    // Encrypt first: if DPAPI fails we must not clobber a previously saved key.
    std::string encrypted;
    if (!s.streamKey.empty()) {
        encrypted = ProtectKey(s.streamKey);
        if (encrypted.empty()) return false;
    }

    std::ofstream f(SettingsPath(), std::ios::trunc);
    if (!f) return false;
    f << "[general]\n";
    f << "monitor=" << s.monitor << "\n";
    f << "window=" << s.windowTitle << "\n";
    f << "backend=" << s.backend << "\n";
    f << "fps=" << s.fps << "\n";
    f << "hevc=" << (s.hevc ? 1 : 0) << "\n";
    f << "cqp=" << s.cqp << "\n";
    f << "preset=" << s.preset << "\n";
    f << "desktopAudio=" << (s.desktopAudio ? 1 : 0) << "\n";
    f << "mic=" << (s.mic ? 1 : 0) << "\n";
    f << "micDevice=" << s.micDevice << "\n";
    f << "desktopDevice=" << s.desktopDevice << "\n";
    f << "webcam=" << (s.webcam ? 1 : 0) << "\n";
    f << "webcamDevice=" << s.webcamDevice << "\n";
    f << "webcamPos=" << s.webcamPos << "\n";
    f << "record=" << (s.record ? 1 : 0) << "\n";
    f << "recordPath=" << s.recordPath << "\n";
    f << "stream=" << (s.stream ? 1 : 0) << "\n";
    f << "platform=" << s.platform << "\n";
    f << "streamKeyEnc=" << encrypted << "\n";
    f << "customUrl=" << s.customUrl << "\n";
    f << "replay=" << (s.replay ? 1 : 0) << "\n";
    f << "replaySeconds=" << s.replaySeconds << "\n";
    f << "replayPrefix=" << s.replayPrefix << "\n";
    f << "chroma=" << (s.chroma ? 1 : 0) << "\n";
    f << "chromaColor=" << s.chromaColor << "\n";
    f << "browser=" << (s.browser ? 1 : 0) << "\n";
    f << "browserUrl=" << s.browserUrl << "\n";
    f << "hotkey=" << s.hotkey << "\n";
    f << "activeScene=" << s.activeScene << "\n";
    f << "sceneCount=" << s.scenes.size() << "\n";
    for (size_t i = 0; i < s.scenes.size(); ++i) {
        const UiSceneSetting& sc = s.scenes[i];
        std::string name = sc.name;
        for (char& c : name)
            if (c == '|' || c == '\n' || c == '\r') c = ' ';
        f << "scene" << i << "=" << name << "|" << (sc.capture ? 1 : 0) << "|"
          << (sc.webcam ? 1 : 0) << "|" << (sc.browser ? 1 : 0) << "|" << (sc.cursor ? 1 : 0)
          << "|" << sc.corner << "\n";
    }
    return true;
}
