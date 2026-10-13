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

// ---- scene serialization ---------------------------------------------------
// v2: "v2|<name>|<cursor>|<src>;<src>;..."
//     src = type:enabled:device:monitor:window:dxgi:corner:camX:camY
// ';', ':' and '|' inside values are stored as spaces (device names and window
// titles are match substrings, so this is lossless in practice).
// v1 (legacy): "<name>|<capture>|<webcam>|<browser>|<cursor>|<corner>|..."

static std::vector<std::string> SplitStr(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

static std::string Sanitize(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        if (c == ':' || c == ';' || c == '|' || c == '\n' || c == '\r') c = ' ';
    return o;
}

static std::string EncodeSource(const UiSourceSetting& s) {
    // type:device:enabled:monitor:window:dxgi:corner:camX:camY
    char num[160];
    sprintf_s(num, "%d:%d:%s:%d:%d:%.4f:%.4f", s.enabled ? 1 : 0, s.monitor,
              Sanitize(s.window).c_str(), s.dxgi ? 1 : 0, s.corner, s.camX, s.camY);
    return std::to_string(s.type) + ":" + Sanitize(s.device) + ":" + num;
}

static bool ParseScene(const std::string& val, UiSceneSetting* sc) {
    std::vector<std::string> parts = SplitStr(val, '|');
    if (!parts.empty() && parts[0] == "v2") {
        if (parts.size() < 2) return false;
        sc->name = parts[1].empty() ? "Scene" : parts[1];
        sc->cursor = (parts.size() > 2 && parts[2] == "0") ? 0 : 1;
        sc->sources.clear();
        if (parts.size() > 3 && !parts[3].empty()) {
            for (const std::string& item : SplitStr(parts[3], ';')) {
                if (item.empty()) continue;
                std::vector<std::string> f = SplitStr(item, ':');
                if (f.size() < 3) continue;
                UiSourceSetting src;
                src.type = atoi(f[0].c_str());
                if (src.type < 0 || src.type > 4) src.type = 0;
                src.device = f[1];
                src.enabled = f[2] == "0" ? 0 : 1;
                if (f.size() > 3) src.monitor = atoi(f[3].c_str());
                if (f.size() > 4) src.window = f[4];
                // Heal files written by an earlier build whose encoder dropped
                // the window field (a bare "0"/"1" is the dxgi flag, not a title).
                if (src.window == "0" || src.window == "1") src.window.clear();
                if (f.size() > 5) src.dxgi = f[5] == "1" ? 1 : 0;
                if (f.size() > 6) src.corner = atoi(f[6].c_str());
                if (f.size() > 7) src.camX = (float)atof(f[7].c_str());
                if (f.size() > 8) src.camY = (float)atof(f[8].c_str());
                if (src.corner < 0 || src.corner > 3) src.corner = 3;
                sc->sources.push_back(src);
            }
        }
        return true;
    }
    // legacy v1
    while (parts.size() < 6) parts.push_back("0");
    sc->name = parts[0].empty() ? "Scene" : parts[0];
    sc->cursor = parts[4] == "0" ? 0 : 1;
    sc->sources.clear();
    UiSourceSetting cap;
    cap.type = 0;
    cap.enabled = parts[1] == "0" ? 0 : 1;
    sc->sources.push_back(cap);
    if (parts[2] == "1") {
        UiSourceSetting v;
        v.type = 1;
        v.corner = atoi(parts[5].c_str());
        if (v.corner < 0 || v.corner > 3) v.corner = 3;
        if (parts.size() > 6) v.camX = (float)atof(parts[6].c_str());
        if (parts.size() > 7) v.camY = (float)atof(parts[7].c_str());
        sc->sources.push_back(v);
    }
    if (parts[3] == "1") {
        UiSourceSetting b;
        b.type = 2;
        sc->sources.push_back(b);
    }
    return true;
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
        else if (key == "gpu") s->gpu = atoi(val.c_str());
        else if (key == "fps") s->fps = (uint32_t)atoi(val.c_str());
        else if (key == "hevc") s->hevc = (val == "1");
        else if (key == "cqp") s->cqp = (uint32_t)atoi(val.c_str());
        else if (key == "recordCbr") s->recordCbr = (val == "1");
        else if (key == "recordBitrate") s->recordBitrate = (uint32_t)atoi(val.c_str());
        else if (key == "bframes") s->bframes = atoi(val.c_str());
        else if (key == "lookahead") s->lookahead = (val == "1");
        else if (key == "multipass") s->multipass = (val == "1");
        else if (key == "preset") s->preset = atoi(val.c_str());
        else if (key == "desktopAudio") s->desktopAudio = (val == "1");
        else if (key == "mic") s->mic = (val == "1");
        else if (key == "micDevice") s->micDevice = val;
        else if (key == "desktopDevice") s->desktopDevice = val;
        else if (key == "micGain") s->micGain = (float)atof(val.c_str());
        else if (key == "desktopGain") s->desktopGain = (float)atof(val.c_str());
        else if (key == "micGate") s->micGate = (val == "1");
        else if (key == "micGateDb") s->micGateDb = atoi(val.c_str());
        else if (key == "micComp") s->micComp = (val == "1");
        else if (key == "micCompDb") s->micCompDb = atoi(val.c_str());
        else if (key == "micCompRatio") s->micCompRatio = atoi(val.c_str());
        else if (key == "monitorMic") s->monitorMic = (val == "1");
        else if (key == "idlePreview") s->idlePreview = (val == "1");
        else if (key == "closeToTray") s->closeToTray = (val == "1");
        else if (key == "accent") s->accent = atoi(val.c_str());
        else if (key == "light") s->light = (val == "1");
        else if (key == "webcam") s->webcam = (val == "1");
        else if (key == "webcamDevice") s->webcamDevice = val;
        else if (key == "webcamPos") s->webcamPos = val;
        else if (key == "record") s->record = (val == "1");
        else if (key == "recordPath") s->recordPath = val;
        else if (key == "stream") s->stream = (val == "1");
        else if (key == "streamBitrate") s->streamBitrate = (uint32_t)atoi(val.c_str());
        else if (key == "streamHevc") s->streamHevc = (val == "1");
        else if (key == "streamReconnect") s->streamReconnect = atoi(val.c_str());
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
                UiSceneSetting sc;
                if (!ParseScene(val, &sc)) sc.name = "Scene";
                if ((int)s->scenes.size() <= idx) s->scenes.resize(idx + 1);
                s->scenes[idx] = sc;
            }
        }
    }

    if (s->scenes.empty()) {
        // First run (or an upgrade from older builds): seed one scene and carry
        // the old global toggles over into sources.
        UiSceneSetting sc;
        sc.name = "Game";
        UiSourceSetting cap;
        cap.type = 0;
        sc.sources.push_back(cap);
        if (s->webcam) {
            UiSourceSetting v;
            v.type = 1;
            sc.sources.push_back(v);
        }
        if (s->browser) {
            UiSourceSetting b;
            b.type = 2;
            sc.sources.push_back(b);
        }
        UiSourceSetting ao;
        ao.type = 4;
        ao.enabled = s->desktopAudio ? 1 : 0;
        ao.device = s->desktopDevice;
        sc.sources.push_back(ao);
        if (s->mic) {
            UiSourceSetting ai;
            ai.type = 3;
            ai.device = s->micDevice;
            sc.sources.push_back(ai);
        }
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
    f << "gpu=" << s.gpu << "\n";
    f << "fps=" << s.fps << "\n";
    f << "hevc=" << (s.hevc ? 1 : 0) << "\n";
    f << "cqp=" << s.cqp << "\n";
    f << "recordCbr=" << (s.recordCbr ? 1 : 0) << "\n";
    f << "recordBitrate=" << s.recordBitrate << "\n";
    f << "bframes=" << s.bframes << "\n";
    f << "lookahead=" << (s.lookahead ? 1 : 0) << "\n";
    f << "multipass=" << (s.multipass ? 1 : 0) << "\n";
    f << "preset=" << s.preset << "\n";
    f << "desktopAudio=" << (s.desktopAudio ? 1 : 0) << "\n";
    f << "mic=" << (s.mic ? 1 : 0) << "\n";
    f << "micDevice=" << s.micDevice << "\n";
    f << "desktopDevice=" << s.desktopDevice << "\n";
    f << "micGain=" << s.micGain << "\n";
    f << "desktopGain=" << s.desktopGain << "\n";
    f << "micGate=" << (s.micGate ? 1 : 0) << "\n";
    f << "micGateDb=" << s.micGateDb << "\n";
    f << "micComp=" << (s.micComp ? 1 : 0) << "\n";
    f << "micCompDb=" << s.micCompDb << "\n";
    f << "micCompRatio=" << s.micCompRatio << "\n";
    f << "monitorMic=" << (s.monitorMic ? 1 : 0) << "\n";
    f << "idlePreview=" << (s.idlePreview ? 1 : 0) << "\n";
    f << "closeToTray=" << (s.closeToTray ? 1 : 0) << "\n";
    f << "accent=" << s.accent << "\n";
    f << "light=" << (s.light ? 1 : 0) << "\n";
    f << "webcam=" << (s.webcam ? 1 : 0) << "\n";
    f << "webcamDevice=" << s.webcamDevice << "\n";
    f << "webcamPos=" << s.webcamPos << "\n";
    f << "record=" << (s.record ? 1 : 0) << "\n";
    f << "recordPath=" << s.recordPath << "\n";
    f << "stream=" << (s.stream ? 1 : 0) << "\n";
    f << "streamBitrate=" << s.streamBitrate << "\n";
    f << "streamHevc=" << (s.streamHevc ? 1 : 0) << "\n";
    f << "streamReconnect=" << s.streamReconnect << "\n";
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
        std::string line = "v2|" + Sanitize(sc.name) + "|" + (sc.cursor ? "1" : "0") + "|";
        for (size_t k = 0; k < sc.sources.size(); ++k) {
            if (k) line += ';';
            line += EncodeSource(sc.sources[k]);
        }
        f << "scene" << i << "=" << line << "\n";
    }
    return true;
}
