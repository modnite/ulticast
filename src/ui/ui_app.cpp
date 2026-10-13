// Ulticast UI -- a dark, Direct2D-rendered control surface for the capture engine.
//
// Design notes:
//  - Single fixed-size window, custom-drawn widgets (no common controls).
//  - Live preview via a shared D3D11 texture produced by the Session.
//  - Owns its own D3D device so the capture thread and the UI never share a
//    command context.
//  - Tray icon: closing the window hides it; Exit lives in the tray menu.

#include "ui_app.hpp"
#include "settings.hpp"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/diag.hpp"
#include "../core/log.hpp"
#include "../core/version.hpp"
#include "../core/wasapi_capture.hpp"
#include "../core/webcam_mf.hpp"
#include "../core/capture_dshow.hpp"
#include "../core/capture_wgc.hpp"
#include "../core/twitch_auth.hpp"
#include "../session.hpp"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------- constants

constexpr wchar_t kClassName[] = L"UlticastUiWindow";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;

constexpr float kWinW = 1060.f;
constexpr float kWinH = 700.f;

// palette -- mutable: ApplyTheme() fills these from the accent choice + light/dark
D2D1_COLOR_F kBg, kPanel, kPanel2, kBorder, kText, kTextDim, kAccent, kAccent2, kRec, kOk, kWarn,
    kWhite, kBlack, kHover, kHoverDown;

struct AccentDef {
    const wchar_t* name;
    uint32_t rgb;
};
const AccentDef kAccents[] = {
    {L"Violet", 0xA855F7}, {L"Cyan", 0x22D3EE},  {L"Emerald", 0x34D399}, {L"Amber", 0xFBBF24},
    {L"Rose", 0xFB7185},   {L"Azure", 0x60A5FA}, {L"Magenta", 0xE879F9}, {L"Slate", 0x94A3B8},
};
constexpr int kAccentCount = (int)(sizeof(kAccents) / sizeof(kAccents[0]));

static D2D1_COLOR_F Hsl(float h, float s, float l) {
    // h in [0,1), s/l in [0,1]
    auto hue2rgb = [](float p, float q, float t) {
        if (t < 0.f) t += 1.f;
        if (t > 1.f) t -= 1.f;
        if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
        if (t < 1.f / 2.f) return q;
        if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
        return p;
    };
    float r, g, b;
    if (s <= 0.f) {
        r = g = b = l;
    } else {
        const float q = l < 0.5f ? l * (1.f + s) : l + s - l * s;
        const float p = 2.f * l - q;
        r = hue2rgb(p, q, h + 1.f / 3.f);
        g = hue2rgb(p, q, h);
        b = hue2rgb(p, q, h - 1.f / 3.f);
    }
    return D2D1::ColorF(r, g, b, 1.0f);
}

static void RgbToHsl(uint32_t rgb, float* h, float* s, float* l) {
    const float r = ((rgb >> 16) & 0xFF) / 255.f, g = ((rgb >> 8) & 0xFF) / 255.f,
                b = (rgb & 0xFF) / 255.f;
    const float mx = (std::max)((std::max)(r, g), b), mn = (std::min)((std::min)(r, g), b);
    *l = (mx + mn) / 2.f;
    if (mx == mn) {
        *h = 0.f;
        *s = 0.f;
        return;
    }
    const float d = mx - mn;
    *s = *l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
    if (mx == r)
        *h = (g - b) / d + (g < b ? 6.f : 0.f);
    else if (mx == g)
        *h = (b - r) / d + 2.f;
    else
        *h = (r - g) / d + 4.f;
    *h /= 6.f;
}

// Fills the palette from an accent + light/dark. Called at startup and whenever
// the user picks a theme in Settings > Appearance.
static void ApplyTheme(int accentIndex, bool light) {
    const int ai = std::clamp(accentIndex, 0, kAccentCount - 1);
    float h = 0.78f, s = 0.8f, l = 0.6f;
    RgbToHsl(kAccents[ai].rgb, &h, &s, &l);
    if (light) {
        kBg = Hsl(h, 0.20f, 0.97f);
        kPanel = Hsl(h, 0.22f, 0.935f);
        kPanel2 = Hsl(h, 0.26f, 0.90f);
        kBorder = Hsl(h, 0.24f, 0.80f);
        kText = Hsl(h, 0.35f, 0.14f);
        kTextDim = Hsl(h, 0.16f, 0.44f);
        kHover = D2D1::ColorF(0.f, 0.f, 0.f, 0.05f);
        kHoverDown = D2D1::ColorF(0.f, 0.f, 0.f, 0.10f);
    } else {
        kBg = Hsl(h, 0.30f, 0.075f);
        kPanel = Hsl(h, 0.28f, 0.115f);
        kPanel2 = Hsl(h, 0.30f, 0.165f);
        kBorder = Hsl(h, 0.28f, 0.27f);
        kText = Hsl(h, 0.22f, 0.93f);
        kTextDim = Hsl(h, 0.15f, 0.62f);
        kHover = D2D1::ColorF(1.f, 1.f, 1.f, 0.06f);
        kHoverDown = D2D1::ColorF(0.f, 0.f, 0.f, 0.16f);
    }
    kAccent = Hsl(h, 0.72f, light ? 0.48f : 0.62f);
    kAccent2 = Hsl(fmodf(h + 0.42f, 1.f), 0.70f, light ? 0.45f : 0.58f);
    kRec = Hsl(0.97f, 0.72f, light ? 0.50f : 0.62f);
    kOk = Hsl(0.42f, 0.60f, light ? 0.38f : 0.52f);
    kWarn = Hsl(0.11f, 0.80f, light ? 0.42f : 0.60f);
    kWhite = D2D1::ColorF(0xFFFFFF);
    kBlack = D2D1::ColorF(0x000000);
}

enum {
    ID_START = 1,
    ID_REC,
    ID_LIVE,
    ID_PAUSE,
    ID_CLIP,
    ID_OPENFOLDER,
    ID_TAB_LIVE,
    ID_TAB_SETUP,
    ID_TAB_RECORD,
    ID_TAB_STREAM,
    ID_TAB_SETTINGS,
    ID_SCENE_ADD,
    ID_SCENE_REMOVE,
    ID_SCENE_NAME,
    ID_SRC_CAPTURE,
    ID_SRC_WEBCAM,
    ID_SRC_BROWSER,
    ID_SRC_CURSOR,
    ID_COMBO_CORNER,
    ID_COMBO_GPU,
    ID_COMBO_RATE,
    ID_COMBO_DESKDEV,
    ID_COMBO_STREAMCODEC,
    ID_SLIDER_RECONNECT,
    ID_TOGGLE_RECORD,
    ID_TOGGLE_STREAM,
    ID_TOGGLE_WEBCAM,
    ID_TOGGLE_DESKTOP,
    ID_TOGGLE_MIC,
    ID_TOGGLE_REPLAY,
    ID_COMBO_MONITOR,
    ID_COMBO_WINDOW,
    ID_COMBO_BACKEND,
    ID_COMBO_FPS,
    ID_COMBO_WEBCAMDEV,
    ID_COMBO_POS,
    ID_COMBO_MICDEV,
    ID_COMBO_CODEC,
    ID_COMBO_PRESET,
    ID_COMBO_PLATFORM,
    ID_SLIDER_CQP,
    ID_SLIDER_REPLAY,
    ID_SLIDER_BFRAMES,
    ID_SLIDER_MICGAIN,
    ID_SLIDER_DESKGAIN,
    ID_TOGGLE_LOOKAHEAD,
    ID_TOGGLE_MULTIPASS,
    ID_TOGGLE_IDLEPREVIEW,
    ID_TOGGLE_TRAYCLOSE,
    ID_TOGGLE_MICGATE,
    ID_SLIDER_GATEDB,
    ID_TOGGLE_COMP,
    ID_SLIDER_COMPDB,
    ID_SLIDER_COMPRATIO,
    ID_TOGGLE_MONITOR,
    ID_FIELD_KEY,
    ID_FIELD_URL,
    ID_FIELD_CHROMA,
    ID_FIELD_BROWSER,
    ID_FIELD_BITRATE,
    ID_FIELD_STREAMBITRATE,
    ID_TOGGLE_CHROMA,
    ID_TOGGLE_BROWSER,
    ID_OPENLOGS,
    ID_REPORT,
    ID_BROWSE,
    ID_ITEM_BASE = 1000,
    ID_SCENE_BASE = 2000,
    ID_PREVIEW_WEBCAM = 3000,
    ID_ADD_SOURCE = 4000,
    ID_SRC_PROP_BASE = 4001,    // + source index: row click opens properties
    ID_SRC_TOGGLE_BASE = 4100,  // + source index: enable/disable
    ID_WINDOW_PICK = 4200,
    ID_ACCENT_BASE = 5000,      // + accent index: theme swatches
    ID_TOGGLE_LIGHT = 5100,
    ID_TWITCH_CONNECT = 5200,
    ID_TWITCH_DISCONNECT,
    ID_FIELD_TWITCHCID,
    ID_BROWSER_TEST,
    ID_BROWSER_RELOAD,
    ID_COMBO_CONTAINER,
    ID_TOGGLE_TRACKS,
    ID_COMBO_HOTKEY,
    ID_TOGGLE_FADE,
    ID_COMBO_STREAMPRESET,
    ID_SLIDER_STREAMBFRAMES,
    ID_TOGGLE_STREAMLOOKAHEAD,
    ID_TOGGLE_STREAMMULTIPASS,
    ID_SRC_UP_BASE = 4300,    // + source index: move up in the list (z-order)
    ID_SRC_DOWN_BASE = 4400,  // + source index: move down
};

enum {
    ID_TRAY_SHOW = 9001,
    ID_TRAY_REC,
    ID_TRAY_LIVE,
    ID_TRAY_PAUSE,
    ID_TRAY_CLIP,
    ID_TRAY_EXIT,
};

enum class BtnStyle { Primary, Ghost, Danger };

struct Hit {
    int id;
    D2D1_RECT_F rect;
    bool enabled;
};

D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float a) {
    return D2D1::ColorF(c.r, c.g, c.b, a);
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

// Swaps a path's extension (the container choice drives it).
static std::string WithExtension(const std::string& path, const std::string& ext) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return path.substr(0, dot + 1) + ext;
    return path + "." + ext;
}

std::wstring FormatTime(double seconds) {    int total = (int)seconds;
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    if (h > 0) return Format(L"%d:%02d:%02d", h, m, s);
    return Format(L"%02d:%02d", m, s);
}

std::wstring ShortPath(const std::string& utf8) {
    std::wstring w = Widen(utf8);
    if (w.size() <= 36) return w;
    return L"…" + w.substr(w.size() - 34);
}

HICON MakeAppIcon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) return nullptr;

    auto* px = (uint32_t*)bits;
    const float c = size / 2.f;
    const float r = size * 0.47f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (x + 0.5f) - c, dy = (y + 0.5f) - c;
            float d = sqrtf(dx * dx + dy * dy);
            float aa = std::clamp(r - d, 0.f, 1.5f);
            uint8_t a = (uint8_t)(aa / 1.5f * 255);
            // violet disc with a white play triangle
            uint32_t colorPx = 0xFF5C7CFF;  // BGRA: violet
            float tx = (x + 0.5f - c) / (size * 0.30f);
            float ty = (y + 0.5f - c) / (size * 0.34f);
            if (tx > -0.6f && tx < 1.0f && fabsf(ty) < (1.0f - tx) * 0.85f) colorPx = 0xFFFFFFFF;
            px[y * size + x] = (colorPx & 0x00FFFFFF) | ((uint32_t)a << 24);
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------------- app

class UiApp {
public:
    UiApp() = default;
    ~UiApp() { Shutdown(); }

    bool Init(HINSTANCE instance);
    int Run();
    void Shutdown();

private:
    // window plumbing
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    // graphics
    bool InitGraphics();
    bool CreateDeviceResources();
    void CreateTextFormats();
    bool CreateTargetBitmap();
    void ResizeSwapChain(UINT w, UINT h);
    ID2D1SolidColorBrush* Br(const D2D1_COLOR_F& c);
    ComPtr<IDWriteTextFormat> MakeFormat(float size, DWRITE_FONT_WEIGHT weight);

    // scale helpers. The layout is designed at kWinW x kWinH "design px"; the
    // window may be any size (DPI scaling, maximize, resize), so everything is
    // zoom-to-fit and centered. Hit testing uses the same mapping.
    D2D1_RECT_F S(float x, float y, float w, float h) const {
        return D2D1::RectF(x * scale_ + offX_, y * scale_ + offY_, (x + w) * scale_ + offX_,
                           (y + h) * scale_ + offY_);
    }
    float Px(float v) const { return v * scale_; }
    D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy) const {
        return D2D1::RectF(r.left + dx * scale_, r.top + dy * scale_, r.right - dx * scale_,
                           r.bottom - dy * scale_);
    }

    // drawing
    void Paint();
    void DrawHeader();
    void DrawModePage(bool recordMode);
    void DrawRecordCards();
    void DrawStreamCards();
    void DrawSettingsPage();
    void DrawSceneRail();
    void DrawPreview(const D2D1_RECT_F& r);
    void DrawPreviewStats(const D2D1_RECT_F& r);
    void DrawTransport(int mode);  // 0 = record, 1 = stream, 2 = both
    void DrawActivityStrip(const D2D1_RECT_F& r, int maxLines);
    void DrawPopup();
    void ApplyCaptionTheme();
    void ApplyThemeNow();  // palette + gradient + caption + repaint
    void Card(float x, float y, float w, float h, const std::wstring& title);
    D2D1_RECT_F PreviewRect() const { return S(320, 64, 720, 286); }
    // preview <-> canvas mapping for the draggable webcam PiP
    D2D1_RECT_F PreviewCanvasToDevice(const D2D1_RECT_F& pr, float cx, float cy, float cw,
                                      float ch) const;
    D2D1_RECT_F PreviewWebcamRect(const D2D1_RECT_F& pr) const;
    int FirstVideoSourceIndex() const;

    void Text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
              const D2D1_COLOR_F& color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void Text(const std::wstring& s, const ComPtr<IDWriteTextFormat>& fmt, const D2D1_RECT_F& r,
              const D2D1_COLOR_F& color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        Text(s, fmt.Get(), r, color, align);
    }
    float MeasureText(const std::wstring& s, IDWriteTextFormat* fmt);
    void Panel(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& fill, bool border);
    void Button(int id, const D2D1_RECT_F& r, const std::wstring& label, BtnStyle style,
                bool enabled = true);
    void Toggle(int id, const D2D1_RECT_F& r, const std::wstring& label, bool* value,
                bool enabled = true, bool pinRight = false);
    void Combo(int id, const D2D1_RECT_F& r, const std::vector<std::wstring>& items, int* sel,
               bool enabled = true);
    void Slider(int id, const D2D1_RECT_F& r, int minV, int maxV, int* value,
                const wchar_t* suffixFmt, float displayScale = 1.0f);
    void SetSliderFromX(int id, float x);
    void Field(int id, const D2D1_RECT_F& r, std::wstring* text, bool password,
               const wchar_t* placeholder);

    // ---- standard single-line text editing (the focused field) -------------
    struct FieldEdit {
        std::wstring* text = nullptr;
        int caret = 0;
        int anchor = 0;   // selection anchor; == caret when nothing is selected
        float scroll = 0; // horizontal offset in device px (keeps the caret visible)
        uint64_t lastClickMs = 0;
        int lastClickField = 0;
    };
    FieldEdit edit_;
    void EditBind(std::wstring* text);
    void EditClearSelection();
    std::wstring* EditSelectedText();  // null when there is no selection
    void EditInsert(wchar_t c);
    void EditDeleteBack();
    void EditDeleteForward();
    void EditMove(int delta, bool select);
    void EditWordMove(int dir, bool select);
    void EditMoveTo(int index, bool select);
    void EditHome(bool select);
    void EditEnd(bool select);
    void EditSelectAll();
    void EditCopy(bool cut);
    void EditPaste();
    void EditSelectWordAt(int index);
    void EditEnsureCaretVisible(float viewWidthDevicePx);
    int EditCaretIndexFromX(float textOriginX, float clickX) const;
    float EditCaretX(int index) const;   // device px offset inside the text
    void AfterFieldEdit();               // per-field filtering / scene-name sync
    std::wstring* FieldTarget(int id);   // bound string for a field id (null if not a field)
    void SectionLabel(const std::wstring& s, float x, float y);
    void Pill(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, const std::wstring& label);

    // data + state
    void ReloadDevices();
    void RefreshPreview();
    void SyncSettingsFromUi();
    void ApplySettingsToUi();
    SessionConfig BuildConfig();
    std::string BuildStreamUrl();
    void StartSession(bool record, bool stream);
    void StopSession();
    void RecordPressed();   // Start/Stop Recording button
    void GoLivePressed();   // Go Live / End Stream button
    void StartPreview();
    void StopPreview();
    void RestartPreview();  // re-apply source settings to an idle preview
    void OnAction(int id);
    void OnTrayCommand(int id);
    void InitTray();
    void ShowTrayMenu();
    void UpdateWindowTitleState();
    void AddLog(const std::wstring& s);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    float scale_ = 1.0f;
    float offX_ = 0, offY_ = 0;      // centering offset (device px)
    float viewW_ = kWinW, viewH_ = kWinH;  // client size in DIPs
    float fit_ = 1.0f;               // zoom factor applied on top of the DPI scale
    float designW_ = kWinW, designH_ = kWinH;  // visible area in design px
    float textScale_ = -1.0f;        // scale_ the text formats were built for
    bool exiting_ = false;

    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> d3dCtx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1Layer> layer_;
    ComPtr<ID2D1LinearGradientBrush> gradPrimary_;
    ComPtr<ID2D1PathGeometry> chevronGeo_;
    ComPtr<ID2D1PathGeometry> playGeo_;
    ComPtr<IDWriteFactory> dwrite_;
    std::unordered_map<uint64_t, ComPtr<ID2D1SolidColorBrush>> brushes_;

    ComPtr<IDWriteTextFormat> fmtTitle_, fmtSection_, fmtBody_, fmtBodyBold_, fmtSmall_, fmtMono_,
        fmtPill_;

    // preview
    ComPtr<ID3D11Texture2D> previewTex_;
    ComPtr<ID2D1Bitmap1> previewBmp_;
    ComPtr<ID3D11Texture2D> lastPreviewTex_;
    uint64_t previewVer_ = 0;
    uint32_t previewW_ = 0, previewH_ = 0;

    // widgets
    std::vector<Hit> hits_;
    int hover_ = 0, press_ = 0, dragging_ = 0, focus_ = 0, openCombo_ = -1;
    std::map<int, D2D1_RECT_F> comboRects_, sliderRects_, fieldRects_;
    std::map<int, std::pair<int, int>> sliderRange_;  // id -> (min, max)
    std::map<int, int*> sliderValue_;                 // id -> bound value
    struct Popup {
        D2D1_RECT_F rect{};
        std::vector<std::wstring> items;
        int* sel = nullptr;
    };
    bool hasPopup_ = false;
    Popup popup_;

    // data
    std::vector<std::wstring> monitors_, windows_, webcams_, mics_, backends_, fpss_, codecs_,
        presets_, positions_, platforms_, gpus_, rates_, desktops_, streamCodecs_, containers_,
        hotkeys_;
    std::vector<int> hotkeyValues_;
    std::vector<std::string> backendKeys_, platformKeys_, posKeys_;
    std::vector<int> gpuValues_;
    std::vector<bool> gpuNvenc_;
    std::vector<uint32_t> fpsValues_;
    std::vector<int> presetValues_;
    std::vector<bool> codecHevc_;
    int selMonitor_ = 0, selWindow_ = 0, selWebcam_ = 0, selMic_ = 0, selBackend_ = 0, selFps_ = 1,
        selCodec_ = 0, selPreset_ = 4, selPos_ = 3, selPlatform_ = 0, selGpu_ = 0, selRate_ = 0,
        selDesk_ = 0, selStreamCodec_ = 0, selContainer_ = 0, selHotkey_ = 0, selStreamPreset_ = 4;
    std::wstring bitrateText_, streamBitrateText_;
    int micGainPct_ = 100, deskGainPct_ = 100;

    // session + settings
    UiSettings settings_;
    Session session_;
    Session::Stats stats_;
    bool sessionRunning_ = false;
    bool errorShown_ = false;
    std::wstring statusText_;
    bool statusError_ = false;
    std::wstring lastTitle_;
    bool lastGateOn_ = false;
    int lastGateDb_ = -45;
    bool lastCompOn_ = false;
    int lastCompDb_ = -18;
    int lastCompRatio_ = 30;
    bool lastMonitor_ = false;
    bool separateTracks_ = false;
    void MoveSource(int index, int delta);
    std::wstring browserStatus_;  // last "[browser] ..." line (load status)
    void EnsureBrowserSource();
    std::wstring keyText_, urlText_, chromaText_, browserText_;

    // pages + scenes
    struct UiSource {
        SessionSourceType type = SessionSourceType::Capture;
        bool enabled = true;
        std::string device;      // Video/Audio: device name ("" = system default)
        int monitor = 0;         // Capture: monitor index
        std::wstring window;     // Capture: window title substring (empty = monitor)
        bool dxgi = false;       // Capture: DXGI duplication instead of WGC
        int corner = 3;          // Video: PiP corner preset
        float camX = -1, camY = -1;  // Video: free PiP position (0..1 of canvas)
    };
    struct UiScene {
        std::wstring name;
        bool cursor = true;
        std::vector<UiSource> sources;
    };
    int page_ = 0;  // 0 = Live, 1 = Setup
    std::vector<UiScene> scenes_;
    int activeScene_ = 0;
    std::wstring sceneName_;
    void SyncSceneName();
    void PushSceneSources();  // send the active scene to a running session (no logging)
    void ApplyLiveScene();
    bool SceneUsesWebcam() const;
    bool SceneUsesBrowser() const;

    // source list helpers
    void AddSourceAt(int menuIndex);
    void ApplySourceProperty(int sourceIndex, int itemIndex);
    void BuildSourceProperties(int sourceIndex);
    std::vector<std::wstring> SourceMenuItems() const;
    std::wstring SourceLabel(const UiSource& s) const;
    std::wstring SourceTag(const UiSource& s) const;
    bool IsSecondaryCapture(int sourceIndex) const;
    void EnsureSceneDefaults(UiScene& scene) const;
    std::vector<SessionSource> SceneSources(const UiScene& sc) const;
    void DrawActionPopup();
    int openAction_ = 0;         // source menu / properties popup (0 = none)
    int actionSource_ = -1;      // source index the properties popup belongs to
    D2D1_RECT_F actionRect_{};   // anchor rect for the popup
    D2D1_RECT_F addBtnRect_{};   // "+ Add" button rect
    D2D1_RECT_F addMenuRect_{};  // wider anchor for the add-source popup
    std::vector<D2D1_RECT_F> sourceRowRects_;
    struct PropItem {
        int kind = 0;       // 0 monitor, 1 window picker, 2 dxgi, 3 video device,
                            // 4 corner, 5 remove, 6 default device, 7 audio device
        std::string value;
        int number = 0;
    };
    std::vector<PropItem> propItems_;
    std::vector<std::wstring> propLabels_;
    std::vector<std::wstring> windowPickItems_;
    bool sourceHintShown_ = false;

    // idle preview (capture-only session while not recording)
    bool previewSession_ = false;

    // Twitch OAuth: the device-code flow runs on a worker thread; Paint picks
    // the results up (single-threaded UI stays untouched).
    enum class TwitchState { Idle, Working, WaitingAuth, Connected, Failed };
    std::atomic<int> twitchState_{0};
    std::mutex twitchMu_;
    std::wstring twitchStatus_;
    std::string twitchUserCode_, twitchVerifyUri_, twitchKey_, twitchRefreshNew_, twitchDisplay_;
    bool twitchOpenActivate_ = false;
    std::thread twitchThread_;
    int lastTwitchState_ = 0;
    std::wstring twitchClientIdText_;
    void StartTwitchConnect();
    void TwitchWorker(std::string clientId, std::string refreshToken, bool refreshOnly);
    void HandleTwitchStateChange();

    // webcam drag on the preview
    bool draggingWebcam_ = false;
    float dragOffX_ = 0, dragOffY_ = 0;

    // popup scrolling (long lists, e.g. the window picker)
    int popupScroll_ = 0;

    // tray
    NOTIFYICONDATAW nid_{};
    HICON icon_ = nullptr;

    // activity strip
    std::mutex logMu_;
    std::deque<std::wstring> logLines_;
};

// ------------------------------------------------------------------- setup

bool UiApp::Init(HINSTANCE instance) {
    instance_ = instance;
    Diag::StartLog("ui");
    icon_ = MakeAppIcon(32);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = icon_;
    wc.hIconSm = icon_;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // Size the window in *physical* pixels for the current DPI: the UI is
    // per-monitor DPI aware, so a fixed 1020x660 window would be clipped on
    // 125%/150% displays (missing transport row, popups cut off).
    const UINT initDpi = GetDpiForSystem();
    const float initScale = (initDpi ? initDpi : 96) / 96.0f;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    LONG cw = (LONG)(kWinW * initScale);
    LONG ch = (LONG)(kWinH * initScale);
    if (work.right - work.left > 200) {
        cw = (std::min)(cw, work.right - work.left - 40);
        ch = (std::min)(ch, work.bottom - work.top - 40);
    }
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const LONG ww = r.right - r.left, wh = r.bottom - r.top;
    const LONG wx = work.left + ((work.right - work.left) - ww) / 2;
    const LONG wy = work.top + ((work.bottom - work.top) - wh) / 2;

    hwnd_ = CreateWindowExW(0, kClassName, L"Ulticast", WS_OVERLAPPEDWINDOW, wx, wy, ww, wh,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    // Match the frame to the dark UI: immersive dark caption + rounded corners.
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    // Purple caption/border to match the theme (Win11; ignored on Win10).
    ApplyCaptionTheme();

    if (!InitGraphics()) {
        MessageBoxW(hwnd_, L"Direct2D initialization failed.", L"Ulticast", MB_ICONERROR);
        return false;
    }

    LoadUiSettings(&settings_);
    ApplyTheme(settings_.accent, settings_.light);  // palette before any drawing
    if (settings_.recordPath.empty()) settings_.recordPath = DefaultRecordingPath();
    ReloadDevices();
    ApplySettingsToUi();
    InitTray();

    session_.SetLogSink([this](const std::string& msg) {
        std::lock_guard<std::mutex> lock(logMu_);
        logLines_.push_back(Widen(msg));
        if (logLines_.size() > 50) logLines_.pop_front();
        if (msg.rfind("[browser]", 0) == 0) browserStatus_ = Widen(msg.substr(9));
    });

    if (settings_.hotkey != 0) RegisterHotKey(hwnd_, 1, 0, settings_.hotkey);
    SetTimer(hwnd_, kTimerId, 66, nullptr);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    if (settings_.idlePreview) StartPreview();
    return true;
}

void UiApp::Shutdown() {
    exiting_ = true;
    if (twitchThread_.joinable()) twitchThread_.join();
    if (hwnd_) {
        KillTimer(hwnd_, kTimerId);
        UnregisterHotKey(hwnd_, 1);
        if (nid_.cbSize) {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            nid_.cbSize = 0;
        }
        if (icon_) {
            DestroyIcon(icon_);
            icon_ = nullptr;
        }
    }
    session_.Stop();
}

int UiApp::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

bool UiApp::InitGraphics() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, d3d_.GetAddressOf(), nullptr,
                                   d3dCtx_.GetAddressOf());
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgi;
    d3d_.As(&dxgi);

    D2D1_FACTORY_OPTIONS fo{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                           (void**)d2dFactory_.GetAddressOf());
    if (FAILED(hr)) return false;
    if (FAILED(d2dFactory_->CreateDevice(dxgi.Get(), d2dDevice_.GetAddressOf()))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                               dc_.GetAddressOf())))
        return false;

    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;

    RECT cr{};
    GetClientRect(hwnd_, &cr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)(cr.right - cr.left);
    sd.Height = (UINT)(cr.bottom - cr.top);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(d3d_.Get(), hwnd_, &sd, nullptr, nullptr,
                                               swap_.GetAddressOf())))
        return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) dpi_ = 96;
    scale_ = dpi_ / 96.0f;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)dwrite_.GetAddressOf())))
        return false;

    if (!CreateTargetBitmap()) return false;
    if (!CreateDeviceResources()) return false;
    CreateTextFormats();
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

bool UiApp::CreateDeviceResources() {
    brushes_.clear();
    layer_.Reset();
    gradPrimary_.Reset();
    chevronGeo_.Reset();
    playGeo_.Reset();

    if (FAILED(dc_->CreateLayer(nullptr, layer_.GetAddressOf()))) return false;

    // Primary-button fill (kept for the theme-tinted accents).
    D2D1_GRADIENT_STOP stops[2] = {};
    stops[0].position = 0.0f;
    stops[0].color = kAccent;
    stops[1].position = 1.0f;
    stops[1].color = kAccent;
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 2, stopCollection.GetAddressOf())))
        dc_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)),
            stopCollection.Get(), gradPrimary_.GetAddressOf());

    // Combo chevron, cached once (drawn with a translation transform).
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(d2dFactory_->CreatePathGeometry(chevronGeo_.GetAddressOf())) &&
        SUCCEEDED(chevronGeo_->Open(sink.GetAddressOf()))) {
        sink->BeginFigure(D2D1::Point2F(Px(-3.5f), Px(-1.5f)), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(0.f, Px(2.5f)));
        sink->AddLine(D2D1::Point2F(Px(3.5f), Px(-1.5f)));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
    }

    // Play triangle for "Go Live" (filled, translated into place when drawn).
    ComPtr<ID2D1GeometrySink> psink;
    if (SUCCEEDED(d2dFactory_->CreatePathGeometry(playGeo_.GetAddressOf())) &&
        SUCCEEDED(playGeo_->Open(psink.GetAddressOf()))) {
        psink->BeginFigure(D2D1::Point2F(0.f, 0.f), D2D1_FIGURE_BEGIN_FILLED);
        psink->AddLine(D2D1::Point2F(0.f, Px(12)));
        psink->AddLine(D2D1::Point2F(Px(11), Px(6)));
        psink->EndFigure(D2D1_FIGURE_END_CLOSED);
        psink->Close();
    }
    return true;
}

void UiApp::CreateTextFormats() {    brushes_.clear();
    fmtTitle_ = MakeFormat(Px(20), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmtSection_ = MakeFormat(Px(11), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmtBody_ = MakeFormat(Px(13), DWRITE_FONT_WEIGHT_NORMAL);
    fmtBodyBold_ = MakeFormat(Px(13), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmtSmall_ = MakeFormat(Px(11.5f), DWRITE_FONT_WEIGHT_NORMAL);
    fmtMono_ = MakeFormat(Px(24), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmtPill_ = MakeFormat(Px(11), DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

ComPtr<IDWriteTextFormat> UiApp::MakeFormat(float size, DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteTextFormat> f;
    const wchar_t* fonts[] = {L"Segoe UI Variable Display", L"Segoe UI", nullptr};
    for (int i = 0; fonts[i]; ++i) {
        if (SUCCEEDED(dwrite_->CreateTextFormat(fonts[i], nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                                                f.GetAddressOf())))
            break;
        f.Reset();
    }
    if (f) {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}

bool UiApp::CreateTargetBitmap() {
    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf())))) return false;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                                target_.GetAddressOf())))
        return false;
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi_, dpi_);
    return true;
}

void UiApp::ResizeSwapChain(UINT w, UINT h) {
    if (!swap_ || w == 0 || h == 0) return;
    dc_->SetTarget(nullptr);
    target_.Reset();
    previewBmp_.Reset();
    if (SUCCEEDED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) CreateTargetBitmap();
}

ID2D1SolidColorBrush* UiApp::Br(const D2D1_COLOR_F& c) {
    uint64_t key = ((uint64_t)(c.a * 255) << 24) | ((uint64_t)(c.r * 255) << 16) |
                   ((uint64_t)(c.g * 255) << 8) | (uint64_t)(c.b * 255);
    auto it = brushes_.find(key);
    if (it != brushes_.end()) return it->second.Get();
    ComPtr<ID2D1SolidColorBrush> b;
    if (FAILED(dc_->CreateSolidColorBrush(c, b.GetAddressOf()))) return nullptr;
    brushes_[key] = b;
    return b.Get();
}

// ------------------------------------------------------------------ widgets

void UiApp::Text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                 const D2D1_COLOR_F& color, DWRITE_TEXT_ALIGNMENT align) {
    if (!fmt || s.empty()) return;
    const float w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, w, h,
                                         layout.GetAddressOf())))
        return;
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    dc_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), Br(color),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float UiApp::MeasureText(const std::wstring& s, IDWriteTextFormat* fmt) {
    if (!fmt || s.empty()) return 0.f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 10000.f, 100.f,
                                         layout.GetAddressOf())))
        return 0.f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void UiApp::Panel(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& fill, bool border) {
    auto rr = D2D1::RoundedRect(r, Px(radius), Px(radius));
    dc_->FillRoundedRectangle(rr, Br(fill));
    if (border) dc_->DrawRoundedRectangle(rr, Br(kBorder), Px(1));
}

void UiApp::Button(int id, const D2D1_RECT_F& r, const std::wstring& label, BtnStyle style,
                   bool enabled) {
    hits_.push_back({id, r, enabled});
    bool hover = enabled && hover_ == id;
    bool down = enabled && press_ == id;
    auto rr = D2D1::RoundedRect(r, Px(10), Px(10));

    if (style == BtnStyle::Primary && enabled) {
        // Flat accent fill + a hairline highlight along the top edge: reads as a
        // solid button without the muddy gradient.
        dc_->FillRoundedRectangle(rr, Br(kAccent));
        dc_->FillRectangle(D2D1::RectF(r.left + Px(11), r.top + Px(1), r.right - Px(11),
                                       r.top + Px(1.6f)),
                           Br(WithAlpha(kWhite, 0.22f)));
        dc_->DrawRoundedRectangle(rr, Br(WithAlpha(kWhite, 0.16f)), Px(1));
        if (hover) dc_->FillRoundedRectangle(rr, Br(kHover));
        if (down) dc_->FillRoundedRectangle(rr, Br(kHoverDown));
    } else if (style == BtnStyle::Danger && enabled) {
        dc_->FillRoundedRectangle(rr, Br(kRec));
        dc_->FillRectangle(D2D1::RectF(r.left + Px(11), r.top + Px(1), r.right - Px(11),
                                       r.top + Px(1.6f)),
                           Br(WithAlpha(kWhite, 0.22f)));
        dc_->DrawRoundedRectangle(rr, Br(WithAlpha(kWhite, 0.16f)), Px(1));
        if (hover) dc_->FillRoundedRectangle(rr, Br(kHover));
        if (down) dc_->FillRoundedRectangle(rr, Br(kHoverDown));
    } else {
        dc_->FillRoundedRectangle(rr, Br(enabled ? kPanel : WithAlpha(kPanel, 0.5f)));
        D2D1_COLOR_F border = !enabled ? WithAlpha(kBorder, 0.45f)
                                       : (hover ? WithAlpha(kAccent, 0.55f) : kBorder);
        dc_->DrawRoundedRectangle(rr, Br(border), Px(1));
        if (hover) dc_->FillRoundedRectangle(rr, Br(kHover));
        if (down) dc_->FillRoundedRectangle(rr, Br(kHoverDown));
    }

    const D2D1_COLOR_F fg = (style == BtnStyle::Ghost)
                                ? (enabled ? kText : WithAlpha(kTextDim, 0.55f))
                                : WithAlpha(kWhite, enabled ? 1.0f : 0.5f);

    // Action buttons carry a glyph (record dot / play / stop square) so the
    // state is readable at a glance; the glyph+label group is centred.
    const bool glyphBtn = enabled && (id == ID_REC || id == ID_LIVE) && style != BtnStyle::Ghost;
    if (glyphBtn) {
        const float gl = Px(12), gap = Px(10);
        const float tw = MeasureText(label, fmtBodyBold_.Get());
        const float gx = (r.left + r.right) / 2 - (gl + gap + tw) / 2;
        const float gy = (r.top + r.bottom) / 2;
        const bool active = (id == ID_REC) ? stats_.recording : stats_.streaming;
        if (active) {
            dc_->FillRectangle(D2D1::RectF(gx, gy - gl / 2, gx + gl, gy + gl / 2), Br(fg));
        } else if (id == ID_REC) {
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(gx + gl / 2, gy), gl / 2, gl / 2),
                             Br(fg));
        } else if (playGeo_) {
            dc_->SetTransform(D2D1::Matrix3x2F::Translation(gx, gy - gl / 2));
            dc_->FillGeometry(playGeo_.Get(), Br(fg));
            dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        Text(label, fmtBodyBold_,
             D2D1::RectF(gx + gl + gap, r.top, r.right - Px(10), r.bottom), fg);
    } else {
        Text(label, fmtBodyBold_, r, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void UiApp::Toggle(int id, const D2D1_RECT_F& r, const std::wstring& label, bool* value,
                   bool enabled, bool pinRight) {
    hits_.push_back({id, r, enabled});
    const float trackW = Px(38), trackH = Px(20);
    // The switch hugs its label (with a 12 px gap) instead of floating at the
    // far end of the row, so it is never visually attached to a neighbour.
    // pinRight keeps it at the row end (source rows with no label).
    const float labelW = MeasureText(label, fmtBody_.Get());
    const float trackX = pinRight ? (r.right - trackW)
                                  : (std::min)(r.right - trackW, r.left + labelW + Px(12));
    D2D1_RECT_F track = D2D1::RectF(trackX, (r.top + r.bottom) / 2 - trackH / 2,
                                    trackX + trackW, (r.top + r.bottom) / 2 + trackH / 2);
    bool hover = enabled && hover_ == id;

    D2D1_RECT_F labelR = D2D1::RectF(r.left, r.top, track.left - Px(8), r.bottom);
    dc_->PushAxisAlignedClip(labelR, D2D1_ANTIALIAS_MODE_ALIASED);
    Text(label, fmtBody_, labelR, enabled ? kText : WithAlpha(kTextDim, 0.6f));
    dc_->PopAxisAlignedClip();

    auto rr = D2D1::RoundedRect(track, trackH / 2, trackH / 2);
    if (*value && enabled) {
        dc_->FillRoundedRectangle(rr, Br(kAccent));
    } else {
        dc_->FillRoundedRectangle(rr, Br(kPanel2));
        dc_->DrawRoundedRectangle(rr, Br(hover ? WithAlpha(kAccent, 0.45f) : kBorder), Px(1));
    }
    float knobR = trackH / 2 - Px(3);
    float knobX = *value ? track.right - trackH / 2 : track.left + trackH / 2;
    D2D1_COLOR_F knob = !enabled ? WithAlpha(kTextDim, 0.4f) : (*value ? kWhite : kTextDim);
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, (track.top + track.bottom) / 2), knobR, knobR),
                     Br(knob));
}

void UiApp::Combo(int id, const D2D1_RECT_F& r, const std::vector<std::wstring>& items, int* sel,
                  bool enabled) {
    hits_.push_back({id, r, enabled});
    comboRects_[id] = r;
    bool hover = enabled && hover_ == id;
    auto rr = D2D1::RoundedRect(r, Px(8), Px(8));
    dc_->FillRoundedRectangle(rr, Br(enabled ? (hover ? kPanel2 : kPanel) : WithAlpha(kPanel, 0.5f)));
    dc_->DrawRoundedRectangle(rr, Br(hover ? WithAlpha(kAccent, 0.5f) : kBorder), Px(1));

    std::wstring label = (sel && *sel >= 0 && *sel < (int)items.size()) ? items[*sel] : L"";
    dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    Text(label, fmtBody_, Inset(r, 12, 0), enabled ? kText : WithAlpha(kTextDim, 0.6f));
    dc_->PopAxisAlignedClip();

    // chevron (cached geometry, translated into place)
    float cx = r.right - Px(16), cy = (r.top + r.bottom) / 2;
    if (chevronGeo_) {
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy));
        dc_->DrawGeometry(chevronGeo_.Get(), Br(WithAlpha(kTextDim, 0.85f)), Px(1.3f));
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    if (openCombo_ == id) {
        hasPopup_ = true;
        popup_.rect = r;
        popup_.items = items;
        popup_.sel = sel;
    }
}

void UiApp::SetSliderFromX(int id, float x) {
    auto it = sliderRects_.find(id);
    if (it == sliderRects_.end()) return;
    auto range = sliderRange_.find(id);
    auto val = sliderValue_.find(id);
    if (range == sliderRange_.end() || val == sliderValue_.end() || !val->second) return;
    const float trackR = it->second.right - Px(64);  // must match Slider()'s track end
    float t = (x - it->second.left) / (trackR - it->second.left);
    t = std::clamp(t, 0.f, 1.f);
    const int minV = range->second.first, maxV = range->second.second;
    *val->second = (int)(minV + t * (maxV - minV) + 0.5f);
}

void UiApp::Slider(int id, const D2D1_RECT_F& r, int minV, int maxV, int* value,
                   const wchar_t* suffixFmt, float displayScale) {
    hits_.push_back({id, r, true});
    sliderRects_[id] = r;
    sliderRange_[id] = {minV, maxV};
    sliderValue_[id] = value;
    float cy = (r.top + r.bottom) / 2;
    float trackR = r.right - Px(64);
    D2D1_RECT_F track = D2D1::RectF(r.left, cy - Px(2), trackR, cy + Px(2));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(track, Px(2), Px(2)), Br(kPanel2));
    float t = (float)(*value - minV) / (float)(maxV - minV);
    t = std::clamp(t, 0.f, 1.f);
    D2D1_RECT_F filled = D2D1::RectF(r.left, cy - Px(2), r.left + (trackR - r.left) * t, cy + Px(2));
    dc_->FillRoundedRectangle(D2D1::RoundedRect(filled, Px(2), Px(2)), Br(kAccent));
    D2D1_POINT_2F knob = D2D1::Point2F(r.left + (trackR - r.left) * t, cy);
    dc_->FillEllipse(D2D1::Ellipse(knob, Px(6), Px(6)), Br(kWhite));
    dc_->DrawEllipse(D2D1::Ellipse(knob, Px(6), Px(6)), Br(WithAlpha(kBlack, 0.28f)), Px(1));
    Text(displayScale == 1.0f ? Format(suffixFmt, *value)
                              : Format(suffixFmt, (double)(*value * displayScale)),
         fmtSmall_, D2D1::RectF(trackR + Px(8), r.top, r.right, r.bottom), kTextDim,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void UiApp::EditBind(std::wstring* text) {
    edit_.text = text;
    edit_.caret = text ? (int)text->size() : 0;
    edit_.anchor = edit_.caret;
    edit_.scroll = 0.f;
}

void UiApp::EditClearSelection() { edit_.anchor = edit_.caret; }

std::wstring* UiApp::EditSelectedText() {
    if (!edit_.text || edit_.anchor == edit_.caret) return nullptr;
    return edit_.text;
}

void UiApp::EditInsert(wchar_t c) {
    if (!edit_.text) return;
    if (edit_.anchor != edit_.caret) {
        const int lo = (std::min)(edit_.anchor, edit_.caret);
        const int hi = (std::max)(edit_.anchor, edit_.caret);
        edit_.text->erase((size_t)lo, (size_t)(hi - lo));
        edit_.caret = lo;
        edit_.anchor = lo;
    }
    const int at = std::clamp(edit_.caret, 0, (int)edit_.text->size());
    edit_.text->insert(edit_.text->begin() + at, c);
    edit_.caret = at + 1;
    edit_.anchor = edit_.caret;
}

void UiApp::EditDeleteBack() {
    if (!edit_.text) return;
    if (edit_.anchor != edit_.caret) {
        const int lo = (std::min)(edit_.anchor, edit_.caret);
        const int hi = (std::max)(edit_.anchor, edit_.caret);
        edit_.text->erase((size_t)lo, (size_t)(hi - lo));
        edit_.caret = lo;
        edit_.anchor = lo;
        return;
    }
    if (edit_.caret > 0) {
        edit_.text->erase((size_t)edit_.caret - 1, 1);
        --edit_.caret;
        edit_.anchor = edit_.caret;
    }
}

void UiApp::EditDeleteForward() {
    if (!edit_.text) return;
    if (edit_.anchor != edit_.caret) {
        EditDeleteBack();
        return;
    }
    if (edit_.caret < (int)edit_.text->size()) edit_.text->erase((size_t)edit_.caret, 1);
}

void UiApp::EditMoveTo(int index, bool select) {
    if (!edit_.text) return;
    edit_.caret = std::clamp(index, 0, (int)edit_.text->size());
    if (!select) edit_.anchor = edit_.caret;
}

void UiApp::EditMove(int delta, bool select) {
    if (!edit_.text) return;
    // Moving with a selection and no shift collapses to the selection edge.
    if (!select && edit_.anchor != edit_.caret) {
        edit_.caret = delta < 0 ? (std::min)(edit_.anchor, edit_.caret)
                                : (std::max)(edit_.anchor, edit_.caret);
        edit_.anchor = edit_.caret;
        return;
    }
    EditMoveTo(edit_.caret + delta, select);
}

void UiApp::EditWordMove(int dir, bool select) {
    if (!edit_.text) return;
    const std::wstring& s = *edit_.text;
    int i = edit_.caret;
    auto isWord = [](wchar_t c) { return iswalnum(c) != 0; };
    if (dir < 0) {
        while (i > 0 && !isWord(s[i - 1])) --i;
        while (i > 0 && isWord(s[i - 1])) --i;
    } else {
        const int n = (int)s.size();
        while (i < n && !isWord(s[i])) ++i;
        while (i < n && isWord(s[i])) ++i;
    }
    EditMoveTo(i, select);
}

void UiApp::EditHome(bool select) { EditMoveTo(0, select); }
void UiApp::EditEnd(bool select) { EditMoveTo((int)(edit_.text ? edit_.text->size() : 0), select); }

void UiApp::EditSelectAll() {
    if (!edit_.text) return;
    edit_.anchor = 0;
    edit_.caret = (int)edit_.text->size();
}

void UiApp::EditCopy(bool cut) {
    if (!edit_.text || edit_.anchor == edit_.caret) return;
    const int lo = (std::min)(edit_.anchor, edit_.caret);
    const int hi = (std::max)(edit_.anchor, edit_.caret);
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        const size_t bytes = (size_t)(hi - lo + 1) * sizeof(wchar_t);
        if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (auto* dst = (wchar_t*)GlobalLock(h)) {
                memcpy(dst, edit_.text->c_str() + lo, (size_t)(hi - lo) * sizeof(wchar_t));
                dst[hi - lo] = 0;
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            }
        }
        CloseClipboard();
    }
    if (cut) {
        edit_.text->erase((size_t)lo, (size_t)(hi - lo));
        edit_.caret = lo;
        edit_.anchor = lo;
    }
}

void UiApp::EditPaste() {
    if (!edit_.text) return;
    if (!OpenClipboard(hwnd_)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* src = (wchar_t*)GlobalLock(h)) {
            std::wstring text = src;
            GlobalUnlock(h);
            text.erase(std::remove_if(text.begin(), text.end(),
                                      [](wchar_t c) { return c < 32 || c == 127; }),
                       text.end());
            for (wchar_t c : text) EditInsert(c);
        }
    }
    CloseClipboard();
}

void UiApp::EditSelectWordAt(int index) {
    if (!edit_.text) return;
    const std::wstring& s = *edit_.text;
    auto isWord = [](wchar_t c) { return iswalnum(c) != 0; };
    int lo = std::clamp(index, 0, (int)s.size());
    int hi = lo;
    while (lo > 0 && isWord(s[lo - 1])) --lo;
    while (hi < (int)s.size() && isWord(s[hi])) ++hi;
    if (lo == hi) {  // not on a word: select nothing
        EditMoveTo(lo, false);
        return;
    }
    edit_.anchor = lo;
    edit_.caret = hi;
}

float UiApp::EditCaretX(int index) const {
    if (!edit_.text) return 0.f;
    std::wstring shown = *edit_.text;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), fmtBody_.Get(),
                                         10000.f, 100.f, layout.GetAddressOf())))
        return 0.f;
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    layout->HitTestTextPosition((UINT32)std::clamp(index, 0, (int)shown.size()), FALSE, &x, &y, &m);
    return x;
}

int UiApp::EditCaretIndexFromX(float textOriginX, float clickX) const {
    if (!edit_.text) return 0;
    std::wstring shown = *edit_.text;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), fmtBody_.Get(),
                                         10000.f, 100.f, layout.GetAddressOf())))
        return 0;
    FLOAT x = (std::max)(0.f, clickX - textOriginX);
    FLOAT y = 8.f;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    layout->HitTestPoint(x, y, &trailing, &inside, &m);
    // `inside` is only about vertical position (always true for a single-line
    // field); the caret index is decided by the trailing flag alone.
    const int index = (int)m.textPosition + (trailing ? (int)m.length : 0);
    return std::clamp(index, 0, (int)shown.size());
}

void UiApp::EditEnsureCaretVisible(float viewWidthDevicePx) {
    const float caretX = EditCaretX(edit_.caret);
    if (caretX - edit_.scroll > viewWidthDevicePx) edit_.scroll = caretX - viewWidthDevicePx;
    if (caretX - edit_.scroll < 0) edit_.scroll = caretX;
    if (edit_.scroll < 0) edit_.scroll = 0;
}

std::wstring* UiApp::FieldTarget(int id) {
    switch (id) {
        case ID_FIELD_KEY: return &keyText_;
        case ID_FIELD_URL: return &urlText_;
        case ID_FIELD_CHROMA: return &chromaText_;
        case ID_FIELD_BROWSER: return &browserText_;
        case ID_SCENE_NAME: return &sceneName_;
        case ID_FIELD_BITRATE: return &bitrateText_;
        case ID_FIELD_STREAMBITRATE: return &streamBitrateText_;
        case ID_FIELD_TWITCHCID: return &twitchClientIdText_;
        default: return nullptr;
    }
}

void UiApp::AfterFieldEdit() {    // Field-specific cleanup, mirroring what a native edit control would enforce.
    if (focus_ == ID_FIELD_CHROMA) {
        std::wstring filtered;
        for (wchar_t ch : chromaText_)
            if (iswxdigit(ch) && filtered.size() < 6) filtered += (wchar_t)towupper(ch);
        if (filtered != chromaText_) {
            chromaText_ = filtered;
            edit_.text = &chromaText_;
            edit_.caret = (std::min)(edit_.caret, (int)chromaText_.size());
            edit_.anchor = edit_.caret;
        }
    } else if (focus_ == ID_FIELD_BITRATE || focus_ == ID_FIELD_STREAMBITRATE) {
        std::wstring* fld = focus_ == ID_FIELD_BITRATE ? &bitrateText_ : &streamBitrateText_;
        std::wstring filtered;
        for (wchar_t ch : *fld)
            if (iswdigit(ch) && filtered.size() < 6) filtered += ch;
        if (filtered != *fld) {
            *fld = filtered;
            edit_.text = fld;
            edit_.caret = (std::min)(edit_.caret, (int)fld->size());
            edit_.anchor = edit_.caret;
        }
    } else if (focus_ == ID_SCENE_NAME && !scenes_.empty()) {
        scenes_[activeScene_].name = sceneName_;
        SyncSettingsFromUi();
        SaveUiSettings(settings_);  // renaming a scene persists immediately
    }
}

void UiApp::Field(int id, const D2D1_RECT_F& r, std::wstring* text, bool password,
                  const wchar_t* placeholder) {
    hits_.push_back({id, r, true});
    fieldRects_[id] = r;
    const bool focused = focus_ == id;
    if (focused && edit_.text != text) EditBind(text);
    auto rr = D2D1::RoundedRect(r, Px(8), Px(8));
    dc_->FillRoundedRectangle(rr, Br(kPanel));
    dc_->DrawRoundedRectangle(rr, Br(focused ? kAccent : kBorder), focused ? Px(1.5f) : Px(1));

    std::wstring shown = *text;
    if (password) shown.assign(shown.size(), L'\x2022');
    const float pad = Px(12);
    float scroll = 0.f;
    if (focused) {
        EditEnsureCaretVisible((r.right - r.left) - pad * 2);
        scroll = edit_.scroll;
    }

    dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    if (shown.empty() && !focused) {
        Text(placeholder, fmtBody_, Inset(r, 12, 0), WithAlpha(kTextDim, 0.8f));
    } else {
        // selection highlight behind the text
        if (focused && edit_.anchor != edit_.caret) {
            const int lo = (std::min)(edit_.anchor, edit_.caret);
            const int hi = (std::max)(edit_.anchor, edit_.caret);
            const float x0 = r.left + pad + EditCaretX(lo) - scroll;
            const float x1 = r.left + pad + EditCaretX(hi) - scroll;
            dc_->FillRectangle(
                D2D1::RectF(x0, r.top + Px(5), x1, r.bottom - Px(5)),
                Br(WithAlpha(kAccent, 0.32f)));
        }
        const float originX = r.left + pad - scroll;
        Text(shown, fmtBody_, D2D1::RectF(originX, r.top, originX + Px(2000), r.bottom), kText);
    }
    if (focused) {
        const float x = r.left + pad + EditCaretX(edit_.caret) - scroll;
        const bool on = (GetTickCount64() / 500) % 2 == 0;
        if (on)
            dc_->FillRectangle(D2D1::RectF(x, r.top + Px(6), x + Px(1.5f), r.bottom - Px(6)),
                               Br(kAccent));
    }
    dc_->PopAxisAlignedClip();
}

void UiApp::SectionLabel(const std::wstring& s, float x, float y) {
    Text(s, fmtSection_, S(x, y, 400, 16), kTextDim);
}

void UiApp::Pill(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, const std::wstring& label) {
    auto rr = D2D1::RoundedRect(r, Px(6), Px(6));
    dc_->FillRoundedRectangle(rr, Br(WithAlpha(color, 0.13f)));
    dc_->DrawRoundedRectangle(rr, Br(WithAlpha(color, 0.40f)), Px(1));
    float cy = (r.top + r.bottom) / 2;
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + Px(11), cy), Px(3.5f), Px(3.5f)),
                     Br(color));
    Text(label, fmtPill_, D2D1::RectF(r.left + Px(21), r.top, r.right - Px(8), r.bottom), color);
}

// -------------------------------------------------------------------- paint

void UiApp::Paint() {
    stats_ = session_.GetStats();
    if (stats_.failed && !errorShown_) {
        errorShown_ = true;
        statusText_ = Widen(stats_.error);
        statusError_ = true;
        session_.Stop();
    }
    sessionRunning_ = stats_.running;
    if (!sessionRunning_) previewSession_ = false;
    if (!sessionRunning_ && !stats_.failed) errorShown_ = false;
    UpdateWindowTitleState();
    HandleTwitchStateChange();

    // Live audio-filter updates (gate/compressor apply immediately).
    if (sessionRunning_ &&
        (settings_.micGate != lastGateOn_ || settings_.micGateDb != lastGateDb_)) {
        lastGateOn_ = settings_.micGate;
        lastGateDb_ = settings_.micGateDb;
        session_.SetAudioGate(settings_.micGate, (float)settings_.micGateDb);
    }
    if (sessionRunning_ && (settings_.micComp != lastCompOn_ ||
                            settings_.micCompDb != lastCompDb_ ||
                            settings_.micCompRatio != lastCompRatio_)) {
        lastCompOn_ = settings_.micComp;
        lastCompDb_ = settings_.micCompDb;
        lastCompRatio_ = settings_.micCompRatio;
        session_.SetAudioCompressor(settings_.micComp, (float)settings_.micCompDb,
                                    settings_.micCompRatio / 10.0f);
    }
    if (sessionRunning_ && settings_.monitorMic != lastMonitor_) {
        lastMonitor_ = settings_.monitorMic;
        session_.SetAudioMonitoring(settings_.monitorMic);
    }

    RefreshPreview();

    // Zoom-to-fit: the design is 1020x660, but the window may be any size
    // (DPI scaling, maximize, resize) — scale to fit and center it.
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    const float dpiScale = (dpi_ ? dpi_ : 96) / 96.0f;
    viewW_ = (float)(cr.right - cr.left) / dpiScale;
    viewH_ = (float)(cr.bottom - cr.top) / dpiScale;
    fit_ = (std::min)(viewW_ / kWinW, viewH_ / kWinH);
    fit_ = std::clamp(fit_, 0.6f, 1.6f);
    scale_ = dpiScale * fit_;
    offX_ = ((float)(cr.right - cr.left) - kWinW * scale_) * 0.5f;
    offY_ = ((float)(cr.bottom - cr.top) - kWinH * scale_) * 0.5f;
    designW_ = viewW_ / fit_;
    designH_ = viewH_ / fit_;
    if (fabsf(scale_ - textScale_) > 0.004f) {
        CreateTextFormats();
        textScale_ = scale_;
    }

    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(kBg);

    hits_.clear();
    comboRects_.clear();
    sliderRects_.clear();
    sliderRange_.clear();
    sliderValue_.clear();
    fieldRects_.clear();
    hasPopup_ = false;

    DrawHeader();
    if (page_ == 2)
        DrawSettingsPage();
    else
        DrawModePage(page_ == 0);
    if (openAction_)
        DrawActionPopup();
    else
        DrawPopup();

    HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        ResizeSwapChain((UINT)(cr.right - cr.left), (UINT)(cr.bottom - cr.top));
        CreateDeviceResources();  // cached brushes/layer/gradient are device-bound
        return;
    }
    swap_->Present(1, 0);
}

void UiApp::DrawHeader() {
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(Px(30), Px(26)), Px(6), Px(6)), Br(kAccent));
    Text(L"Ulticast", fmtTitle_, S(46, 0, 190, 52), kText);

    // page tabs
    auto tab = [&](int id, float x, float w, const wchar_t* label, bool active) {
        D2D1_RECT_F r = S(x, 13, w, 28);
        hits_.push_back({id, r, true});
        auto rr = D2D1::RoundedRect(r, Px(7), Px(7));
        if (active)
            dc_->FillRoundedRectangle(rr, Br(WithAlpha(kAccent, 0.25f)));
        else if (hover_ == id)
            dc_->FillRoundedRectangle(rr, Br(kPanel2));
        Text(label, fmtBody_, r, active ? kText : kTextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    tab(ID_TAB_RECORD, 238, 92, L"Record", page_ == 0);
    tab(ID_TAB_STREAM, 336, 88, L"Stream", page_ == 1);
    tab(ID_TAB_SETTINGS, 430, 100, L"Settings", page_ == 2);

    std::wstring label;
    D2D1_COLOR_F color;
    if (stats_.failed) {
        label = L"ERROR";
        color = kRec;
    } else if (!sessionRunning_) {
        label = L"IDLE";
        color = kTextDim;
    } else if (stats_.recording && stats_.streaming) {
        label = L"REC + LIVE";
        color = kRec;
    } else if (stats_.recording) {
        label = L"RECORDING";
        color = kRec;
    } else if (stats_.streaming) {
        label = L"LIVE";
        color = kAccent2;
    } else if (stats_.replayArmed) {
        label = L"REPLAY";
        color = kAccent;
    } else if (previewSession_) {
        label = L"PREVIEW";
        color = kAccent;
    } else {
        label = L"IDLE";
        color = kTextDim;
    }
    float pillW = MeasureText(label, fmtPill_.Get()) + Px(29);
    D2D1_RECT_F pillR =
        D2D1::RectF(Px(kWinW - 20) - pillW, Px(14), Px(kWinW - 20), Px(14) + Px(24));
    Pill(pillR, color, label);
    dc_->FillRectangle(S(0, 51, kWinW, 1), Br(kBorder));}

D2D1_RECT_F UiApp::PreviewCanvasToDevice(const D2D1_RECT_F& pr, float cx, float cy, float cw,
                                         float ch) const {
    if (previewW_ == 0 || previewH_ == 0) return D2D1::RectF();
    const float sc = (std::min)((pr.right - pr.left) / (float)previewW_,
                                (pr.bottom - pr.top) / (float)previewH_);
    const float dispW = previewW_ * sc, dispH = previewH_ * sc;
    const float ox = (pr.left + pr.right) / 2 - dispW / 2;
    const float oy = (pr.top + pr.bottom) / 2 - dispH / 2;
    return D2D1::RectF(ox + cx * sc, oy + cy * sc, ox + (cx + cw) * sc, oy + (cy + ch) * sc);
}

int UiApp::FirstVideoSourceIndex() const {
    if (scenes_.empty()) return -1;
    const UiScene& s = scenes_[activeScene_];
    for (size_t i = 0; i < s.sources.size(); ++i)
        if (s.sources[i].type == SessionSourceType::Webcam) return (int)i;
    return -1;
}

D2D1_RECT_F UiApp::PreviewWebcamRect(const D2D1_RECT_F& pr) const {
    if (scenes_.empty() || stats_.width == 0 || stats_.height == 0) return D2D1::RectF();
    const int vi = FirstVideoSourceIndex();
    if (vi < 0) return D2D1::RectF();
    const UiSource& s = scenes_[activeScene_].sources[vi];
    const float canvasW = (float)stats_.width, canvasH = (float)stats_.height;
    const float cw = canvasW / 4.0f;
    const float ch = cw * 3.0f / 4.0f;  // 4:3 approximation for hit testing
    float x, y;
    if (s.camX >= 0 && s.camY >= 0) {
        x = s.camX * canvasW;
        y = s.camY * canvasH;
    } else {
        const float margin = 16.f;
        const bool left = s.corner == 0 || s.corner == 2;
        const bool top = s.corner == 0 || s.corner == 1;
        x = left ? margin : canvasW - cw - margin;
        y = top ? margin : canvasH - ch - margin;
    }
    return PreviewCanvasToDevice(pr, x, y, cw, ch);
}

void UiApp::DrawPreview(const D2D1_RECT_F& r) {
    Panel(r, 12, kPanel, true);

    if (previewBmp_) {
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(Inset(r, 1, 1), Px(11), Px(11)), geo.GetAddressOf()))) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
                Inset(r, 1, 1), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc_->PushLayer(&lp, layer_.Get());
            auto sz = previewBmp_->GetSize();
            float sc = (std::min)((r.right - r.left) / sz.width, (r.bottom - r.top) / sz.height);
            float w = sz.width * sc, h = sz.height * sc;
            D2D1_RECT_F dst = D2D1::RectF((r.left + r.right) / 2 - w / 2,
                                          (r.top + r.bottom) / 2 - h / 2,
                                          (r.left + r.right) / 2 + w / 2,
                                          (r.top + r.bottom) / 2 + h / 2);
            dc_->DrawBitmap(previewBmp_.Get(), dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            dc_->PopLayer();
        }
    } else {
        float cy = (r.top + r.bottom) / 2;
        D2D1_RECT_F c1 = D2D1::RectF(r.left, cy - Px(24), r.right, cy + Px(2));
        D2D1_RECT_F c2 = D2D1::RectF(r.left, cy + Px(2), r.right, cy + Px(26));
        Text(L"NO SIGNAL", fmtSection_, c1, kTextDim, DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(settings_.idlePreview ? L"Starting preview…"
                                   : L"Idle preview is off — see Setup > Idle preview",
             fmtSmall_, c2, WithAlpha(kTextDim, 0.7f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    if (sessionRunning_ && stats_.width) {
        double fps = stats_.elapsed > 0.1 ? stats_.captured / stats_.elapsed : 0.0;
        std::wstring info = Format(L"%u x %u   ·   %.0f fps", stats_.width, stats_.height, fps);
        if (stats_.dropped)
            info += Format(L"   ·   %llu dropped", (unsigned long long)stats_.dropped);
        float w = MeasureText(info, fmtSmall_.Get()) + Px(24);
        D2D1_RECT_F pill = D2D1::RectF(r.right - Px(12) - w, r.top + Px(12), r.right - Px(12),
                                       r.top + Px(12) + Px(24));
        dc_->FillRoundedRectangle(D2D1::RoundedRect(pill, Px(6), Px(6)),
                                  Br(WithAlpha(kBlack, 0.55f)));
        dc_->DrawRoundedRectangle(D2D1::RoundedRect(pill, Px(6), Px(6)),
                                  Br(WithAlpha(kWhite, 0.10f)), Px(1));
        Text(info, fmtSmall_, D2D1::RectF(pill.left + Px(12), pill.top, pill.right - Px(12),
                                          pill.bottom),
             kText);
    }

    if (sessionRunning_ && !stats_.paused) {
        if (previewSession_) {
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + Px(20), r.top + Px(20)), Px(6),
                                           Px(6)),
                             Br(WithAlpha(kAccent, 0.85f)));
        } else {
            float pulse = 0.55f + 0.45f * (float)sin(GetTickCount64() / 320.0);
            D2D1_COLOR_F dot = WithAlpha(settings_.stream ? kAccent2 : kRec, pulse);
            dc_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(r.left + Px(20), r.top + Px(20)), Px(6), Px(6)),
                Br(dot));
        }
    }

    // Draggable webcam PiP (only while a real session has the camera up).
    if (!previewSession_ && sessionRunning_ && FirstVideoSourceIndex() >= 0 &&
        !stats_.webcamName.empty()) {
        D2D1_RECT_F wr = PreviewWebcamRect(r);
        if (wr.right > wr.left && wr.bottom > wr.top) {
            hits_.push_back({ID_PREVIEW_WEBCAM, wr, true});
            if (draggingWebcam_ || hover_ == ID_PREVIEW_WEBCAM) {
                dc_->DrawRoundedRectangle(D2D1::RoundedRect(wr, Px(4), Px(4)),
                                          Br(WithAlpha(kAccent, 0.9f)), Px(2));
                std::wstring hint = L"Drag to move";
                float tw = MeasureText(hint, fmtSmall_.Get()) + Px(18);
                D2D1_RECT_F tr =
                    D2D1::RectF(wr.left, wr.bottom + Px(6), wr.left + tw, wr.bottom + Px(26));
                dc_->FillRoundedRectangle(D2D1::RoundedRect(tr, Px(6), Px(6)),
                                          Br(WithAlpha(kBlack, 0.65f)));
                Text(hint, fmtSmall_, tr, kText, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }
}

void UiApp::DrawModePage(bool recordMode) {
    DrawSceneRail();

    D2D1_RECT_F preview = PreviewRect();
    DrawPreview(preview);
    DrawPreviewStats(preview);

    if (recordMode)
        DrawRecordCards();
    else
        DrawStreamCards();

    DrawTransport(recordMode ? 0 : 1);
}

void UiApp::DrawSceneRail() {
    const float x = 20, w = 280;
    SectionLabel(L"SCENES", x, 64);

    const int n = (int)scenes_.size();
    Panel(S(x, 82, w, n * 36 + 4), 10, WithAlpha(kPanel, 0.35f), false);
    float y = 86;
    for (int i = 0; i < n && i < 6; ++i) {
        const int id = ID_SCENE_BASE + i;
        D2D1_RECT_F row = S(x, y, w, 32);
        hits_.push_back({id, row, true});
        const bool active = i == activeScene_;
        auto rr = D2D1::RoundedRect(row, Px(8), Px(8));
        if (active)
            dc_->FillRoundedRectangle(rr, Br(WithAlpha(kAccent, 0.28f)));
        else if (hover_ == id)
            dc_->FillRoundedRectangle(rr, Br(kPanel2));
        else
            dc_->FillRoundedRectangle(rr, Br(WithAlpha(kPanel, 0.7f)));
        if (active) dc_->DrawRoundedRectangle(rr, Br(WithAlpha(kAccent, 0.6f)), Px(1));
        dc_->PushAxisAlignedClip(row, D2D1_ANTIALIAS_MODE_ALIASED);
        Text(scenes_[i].name, fmtBody_, Inset(row, 12, 0), active ? kText : kTextDim);
        dc_->PopAxisAlignedClip();
        y += 36;
    }

    // name + add/remove
    y = 86 + n * 36 + 8;
    Field(ID_SCENE_NAME, S(x, y, 180, 34), &sceneName_, false, L"Scene name");
    Button(ID_SCENE_ADD, S(x + 188, y, 44, 34), L"+", BtnStyle::Ghost, scenes_.size() < 6);
    Button(ID_SCENE_REMOVE, S(x + 240, y, 40, 34), L"-", BtnStyle::Ghost, scenes_.size() > 1);

    // ---- sources (the active scene's list)
    y += 34 + 22;
    SectionLabel(L"SOURCES", x, y);
    addBtnRect_ = S(x + 196, y - 5, 84, 24);
    addMenuRect_ = S(x, y - 5, w, 24);
    Button(ID_ADD_SOURCE, addBtnRect_, L"+ Add", BtnStyle::Ghost);
    y += 26;

    const UiScene& sc = scenes_[activeScene_];
    sourceRowRects_.assign(sc.sources.size(), D2D1_RECT_F{});
    for (size_t i = 0; i < sc.sources.size(); ++i) {
        if (y > 484) break;  // leave room for the cursor row + activity card
        D2D1_RECT_F row = S(x, y, w, 32);
        sourceRowRects_[i] = row;
        const int propId = ID_SRC_PROP_BASE + (int)i;
        hits_.push_back({propId, row, true});
        auto rr = D2D1::RoundedRect(row, Px(8), Px(8));
        dc_->FillRoundedRectangle(rr, Br(hover_ == propId ? kPanel2 : WithAlpha(kPanel, 0.7f)));
        dc_->DrawRoundedRectangle(rr, Br(kBorder), Px(1));
        Text(SourceTag(sc.sources[i]), fmtPill_, S(x + 8, y, 40, 32), kAccent,
             DWRITE_TEXT_ALIGNMENT_CENTER);
        dc_->PushAxisAlignedClip(S(x + 52, y, w - 52 - 120, 32), D2D1_ANTIALIAS_MODE_ALIASED);
        Text(SourceLabel(sc.sources[i]), fmtSmall_, S(x + 52, y, w - 52 - 120, 32),
             sc.sources[i].enabled ? kText : WithAlpha(kTextDim, 0.6f));
        dc_->PopAxisAlignedClip();

        // z-order: chevrons move the source up/down (captures stay pinned so the
        // primary capture never changes under a running session).
        const bool isCapture = sc.sources[i].type == SessionSourceType::Capture;
        D2D1_RECT_F upR = S(x + w - 94, y + 5, 18, 22);
        D2D1_RECT_F dnR = S(x + w - 74, y + 5, 18, 22);
        const bool canUp = !isCapture && i > 0;
        const bool canDown = !isCapture && i + 1 < sc.sources.size();
        if (canUp) hits_.push_back({ID_SRC_UP_BASE + (int)i, upR, true});
        if (canDown) hits_.push_back({ID_SRC_DOWN_BASE + (int)i, dnR, true});
        Text(L"\x25B2", fmtSmall_, upR, canUp ? kTextDim : WithAlpha(kTextDim, 0.25f),
             DWRITE_TEXT_ALIGNMENT_CENTER);
        Text(L"\x25BC", fmtSmall_, dnR, canDown ? kTextDim : WithAlpha(kTextDim, 0.25f),
             DWRITE_TEXT_ALIGNMENT_CENTER);

        Toggle(ID_SRC_TOGGLE_BASE + (int)i, S(x + w - 54, y, 50, 32), L"",
               &scenes_[activeScene_].sources[i].enabled, true, /*pinRight=*/true);
        y += 36;
    }

    // cursor is a scene-level source-like switch
    if (y <= 520) {
        Toggle(ID_SRC_CURSOR, S(x, y, w, 32), L"Show Cursor", &scenes_[activeScene_].cursor);
        y += 36;
    }

    // ACTIVITY fills whatever rail space is left (no dead space).
    const float actTop = y + 8;
    if (actTop < 624 - 70) {
        Card(x, actTop, w, 624 - actTop, L"ACTIVITY");
        int lines = (int)((624 - actTop - 36) / 14);
        if (lines < 1) lines = 1;
        DrawActivityStrip(S(x + 10, actTop + 30, w - 20, (float)lines * 14), lines);
    }
}

std::wstring UiApp::SourceTag(const UiSource& s) const {
    switch (s.type) {
        case SessionSourceType::Capture:
            if (!s.window.empty()) return L"WIN";
            return s.dxgi ? L"GAME" : L"DISP";
        case SessionSourceType::Webcam: return L"VID";
        case SessionSourceType::Browser: return L"WEB";
        case SessionSourceType::AudioIn: return L"MIC";
        case SessionSourceType::AudioOut: return L"SYS";
    }
    return L"?";
}

std::wstring UiApp::SourceLabel(const UiSource& s) const {
    switch (s.type) {
        case SessionSourceType::Capture:
            if (!s.window.empty()) return s.window;
            if (s.monitor >= 0 && s.monitor < (int)monitors_.size()) return monitors_[s.monitor];
            return Format(L"Display %d", s.monitor + 1);
        case SessionSourceType::Webcam:
            return s.device.empty() ? L"Default camera / capture card" : Widen(s.device);
        case SessionSourceType::Browser:
            return browserText_.empty() ? L"Browser overlay" : L"Browser: " + browserText_;
        case SessionSourceType::AudioIn:
            return s.device.empty() ? L"Default microphone" : Widen(s.device);
        case SessionSourceType::AudioOut:
            return s.device.empty() ? L"Desktop audio (default output)" : Widen(s.device);
    }
    return L"Source";
}

bool UiApp::IsSecondaryCapture(int sourceIndex) const {
    if (scenes_.empty()) return false;
    const UiScene& sc = scenes_[activeScene_];
    if (sourceIndex < 0 || sourceIndex >= (int)sc.sources.size()) return false;
    if (sc.sources[sourceIndex].type != SessionSourceType::Capture) return false;
    for (int i = 0; i < sourceIndex; ++i)
        if (sc.sources[i].type == SessionSourceType::Capture) return true;
    return false;
}

std::vector<std::wstring> UiApp::SourceMenuItems() const {    return {L"Display Capture (monitor)", L"Window Capture", L"Game Capture (DXGI)",
            L"Video Capture Device (camera, capture card)", L"Microphone",
            L"Desktop Audio (system sound)", L"Browser Overlay"};
}

void UiApp::BuildSourceProperties(int sourceIndex) {
    propItems_.clear();
    propLabels_.clear();
    if (sourceIndex < 0 || sourceIndex >= (int)scenes_[activeScene_].sources.size()) return;
    const UiSource& s = scenes_[activeScene_].sources[sourceIndex];
    auto add = [&](int kind, const std::wstring& label, const std::string& value = std::string(),
                   int number = 0) {
        propItems_.push_back({kind, value, number});
        propLabels_.push_back(label);
    };
    switch (s.type) {
        case SessionSourceType::Capture:
            for (size_t i = 0; i < monitors_.size(); ++i) add(0, monitors_[i], "", (int)i);
            add(1, s.window.empty() ? L"Window capture: pick a window…"
                                    : L"Window: " + s.window);
            add(2, s.dxgi ? L"Game mode (DXGI duplication): on"
                          : L"Game mode (DXGI duplication): off");
            if (IsSecondaryCapture(sourceIndex)) {
                for (int c = 0; c < 4; ++c) {
                    std::wstring label = L"Position: " + positions_[c];
                    if (c == s.corner) label += L"  (current)";
                    add(4, label, "", c);
                }
            }
            add(5, L"Remove source");
            break;
        case SessionSourceType::Webcam: {
            add(6, L"Default camera / capture card");
            for (size_t i = 1; i < webcams_.size(); ++i) add(3, webcams_[i], Narrow(webcams_[i]));
            for (int c = 0; c < 4; ++c) {
                std::wstring label = L"Position: " + positions_[c];
                if (c == s.corner) label += L"  (current)";
                add(4, label, "", c);
            }
            add(5, L"Remove source");
            break;
        }
        case SessionSourceType::AudioIn:
            add(6, L"Default microphone");
            for (size_t i = 1; i < mics_.size(); ++i) add(7, mics_[i], Narrow(mics_[i]));
            add(5, L"Remove source");
            break;
        case SessionSourceType::AudioOut:
            add(6, L"Default output (desktop audio)");
            for (size_t i = 1; i < desktops_.size(); ++i) add(7, desktops_[i], Narrow(desktops_[i]));
            add(5, L"Remove source");
            break;
        case SessionSourceType::Browser:
            add(8, browserText_.empty()
                       ? L"Set the overlay URL in Settings…"
                       : L"Overlay: " + browserText_ + L"   (open Settings…)");
            add(5, L"Remove source");
            break;
    }
}

void UiApp::ApplySourceProperty(int sourceIndex, int itemIndex) {
    if (sourceIndex < 0 || sourceIndex >= (int)scenes_[activeScene_].sources.size()) return;
    if (itemIndex < 0 || itemIndex >= (int)propItems_.size()) return;
    UiSource& s = scenes_[activeScene_].sources[sourceIndex];
    const PropItem& it = propItems_[itemIndex];
    bool captureChanged = false;
    switch (it.kind) {
        case 0:  // monitor
            s.monitor = it.number;
            s.window.clear();
            captureChanged = true;
            break;
        case 1: {  // window picker (second step)
            windowPickItems_.clear();
            windowPickItems_.push_back(L"Full monitor (no window)");
            for (auto& t : WgcCapture::ListWindows()) windowPickItems_.push_back(t);
            actionSource_ = sourceIndex;
            openAction_ = ID_WINDOW_PICK;
            actionRect_ = actionRect_;
            return;  // the popup opens; no other change yet
        }
        case 2:  // DXGI toggle
            s.dxgi = !s.dxgi;
            captureChanged = true;
            break;
        case 3:  // video device
            s.device = it.value;
            break;
        case 4:  // PiP corner
            s.corner = it.number;
            s.camX = s.camY = -1;
            break;
        case 5:  // remove
            scenes_[activeScene_].sources.erase(scenes_[activeScene_].sources.begin() +
                                                sourceIndex);
            break;
        case 6:  // default device
            s.device.clear();
            break;
        case 7:  // audio device
            s.device = it.value;
            break;
        case 8:  // jump to the overlay URL in Settings
            page_ = 2;
            openAction_ = 0;
            return;
    }
    SyncSettingsFromUi();
    SaveUiSettings(settings_);
    PushSceneSources();
    RestartPreview();
    if (captureChanged && sessionRunning_ && !previewSession_) {
        statusText_ = L"Capture source changed — press Stop/Start to apply";
        statusError_ = false;
    }
}

void UiApp::AddSourceAt(int menuIndex) {
    if (scenes_.empty()) return;
    UiScene& sc = scenes_[activeScene_];
    auto findCapture = [&]() -> int {
        for (size_t i = 0; i < sc.sources.size(); ++i)
            if (sc.sources[i].type == SessionSourceType::Capture) return (int)i;
        return -1;
    };
    auto has = [&](SessionSourceType t) {
        for (const UiSource& s : sc.sources)
            if (s.type == t) return true;
        return false;
    };
    UiSource s;
    switch (menuIndex) {
        case 0:  // display
        case 1:  // window
        case 2: {  // game (DXGI)
            s.type = SessionSourceType::Capture;
            s.monitor = selMonitor_;
            s.dxgi = menuIndex == 2;
            if (menuIndex == 1 && selWindow_ > 0 && selWindow_ < (int)windows_.size())
                s.window = windows_[selWindow_];
            s.corner = 3;
            int captures = 0;
            for (const UiSource& src : sc.sources)
                if (src.type == SessionSourceType::Capture) ++captures;
            sc.sources.push_back(s);
            statusText_ = captures == 0 ? L"Capture source added"
                                        : L"Capture added as a picture-in-picture layer";
            statusError_ = false;
            break;
        }
        case 3:  // video capture device (camera, capture card)
            s.type = SessionSourceType::Webcam;
            s.device = settings_.webcamDevice;
            s.corner = 3;
            sc.sources.push_back(s);
            statusText_ = L"Video source added — click its row to pick a device";
            statusError_ = false;
            break;
        case 4:  // microphone
            if (!has(SessionSourceType::AudioIn)) {
                s.type = SessionSourceType::AudioIn;
                s.device = settings_.micDevice;
                sc.sources.push_back(s);
            }
            break;
        case 5:  // desktop audio
            if (!has(SessionSourceType::AudioOut)) {
                s.type = SessionSourceType::AudioOut;
                s.device = settings_.desktopDevice;
                sc.sources.push_back(s);
            }
            break;
        case 6:  // browser overlay
            if (!has(SessionSourceType::Browser)) {
                s.type = SessionSourceType::Browser;
                sc.sources.push_back(s);
            }
            break;
    }
    SyncSettingsFromUi();
    SaveUiSettings(settings_);
    PushSceneSources();
    RestartPreview();
    if (menuIndex <= 2 && sessionRunning_ && !previewSession_) {
        statusText_ = L"Capture source changed — press Stop/Start to apply";
        statusError_ = false;
    }
}

void UiApp::ApplyCaptionTheme() {
    if (!hwnd_) return;
    auto toRef = [](const D2D1_COLOR_F& c) {
        return RGB((int)(c.r * 255.f + 0.5f), (int)(c.g * 255.f + 0.5f), (int)(c.b * 255.f + 0.5f));
    };
    COLORREF caption = toRef(kPanel);
    DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    COLORREF borderColor = toRef(kBorder);
    DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &borderColor, sizeof(borderColor));
    BOOL darkMode = settings_.light ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
}

void UiApp::ApplyThemeNow() {
    ApplyTheme(settings_.accent, settings_.light);
    CreateDeviceResources();  // the primary-button gradient follows the accent
    ApplyCaptionTheme();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiApp::StartTwitchConnect() {
    const int st = twitchState_.load();
    if (st == (int)TwitchState::Working || st == (int)TwitchState::WaitingAuth) return;
    if (twitchThread_.joinable()) twitchThread_.join();

    SyncSettingsFromUi();
    SaveUiSettings(settings_);
    const std::string clientId =
        settings_.twitchClientId.empty() ? Twitch::kDefaultClientId : settings_.twitchClientId;
    if (clientId.empty()) {
        statusText_ = L"Set a Twitch Client ID first";
        statusError_ = true;
        return;
    }
    // With a stored refresh token the flow is silent: refresh, then re-fetch
    // the stream key (Twitch rotates keys only on request, but this keeps the
    // token fresh and confirms the link still works).
    const bool refreshOnly = !settings_.twitchRefreshToken.empty();
    {
        std::lock_guard<std::mutex> lk(twitchMu_);
        twitchStatus_ = refreshOnly ? L"Refreshing Twitch token…" : L"Requesting a device code…";
    }
    twitchThread_ = std::thread(&UiApp::TwitchWorker, this, clientId,
                                settings_.twitchRefreshToken, refreshOnly);
}

void UiApp::TwitchWorker(std::string clientId, std::string refreshToken, bool refreshOnly) {
    auto setStatus = [&](const std::wstring& s) {
        std::lock_guard<std::mutex> lk(twitchMu_);
        twitchStatus_ = s;
    };
    auto setState = [&](TwitchState st) { twitchState_.store((int)st); };

    Twitch::Tokens tokens;
    std::string err;
    if (refreshOnly) {
        setState(TwitchState::Working);
        if (!Twitch::Refresh(clientId, refreshToken, &tokens, &err)) {
            setStatus(L"Twitch refresh failed: " + Widen(err));
            setState(TwitchState::Failed);
            return;
        }
    } else {
        setState(TwitchState::Working);
        Twitch::DeviceCode code;
        if (!Twitch::RequestDeviceCode(clientId, &code, &err)) {
            setStatus(L"Twitch connect failed: " + Widen(err));
            setState(TwitchState::Failed);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(twitchMu_);
            twitchUserCode_ = code.userCode;
            twitchVerifyUri_ = code.verificationUri;
            twitchStatus_ = L"Code " + Widen(code.userCode) + L" at twitch.tv/activate";
        }
        twitchOpenActivate_ = true;
        setState(TwitchState::WaitingAuth);

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(code.expiresIn);
        int interval = code.interval;
        for (;;) {
            // Interruptible sleep so closing the app never waits on the poll.
            for (int i = 0; i < interval * 10 && !exiting_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (exiting_) return;
            if (std::chrono::steady_clock::now() > deadline) {
                setStatus(L"Twitch code expired — try again");
                setState(TwitchState::Failed);
                return;
            }
            const Twitch::Poll p = Twitch::PollToken(clientId, code, &tokens, &err);
            if (p == Twitch::Poll::Authorized) break;
            if (p == Twitch::Poll::Pending) continue;
            if (p == Twitch::Poll::SlowDown) {
                interval += 2;
                continue;
            }
            setStatus(p == Twitch::Poll::Denied ? L"Twitch authorization was denied"
                                                : L"Twitch connect failed: " + Widen(err));
            setState(TwitchState::Failed);
            return;
        }
    }

    setState(TwitchState::Working);
    setStatus(L"Fetching your channel and stream key…");
    if (!Twitch::FetchUser(clientId, &tokens, &err)) {
        setStatus(L"Twitch user lookup failed: " + Widen(err));
        setState(TwitchState::Failed);
        return;
    }
    std::string key;
    if (!Twitch::FetchStreamKey(clientId, tokens, &key, &err)) {
        setStatus(L"Stream key lookup failed: " + Widen(err));
        setState(TwitchState::Failed);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(twitchMu_);
        twitchKey_ = key;
        twitchRefreshNew_ = tokens.refresh;
        twitchDisplay_ = tokens.displayName;
        twitchStatus_ = L"Connected as " + Widen(tokens.displayName);
    }
    setState(TwitchState::Connected);
}

void UiApp::HandleTwitchStateChange() {
    const int st = twitchState_.load();
    if (st == lastTwitchState_) return;
    lastTwitchState_ = st;
    if (st == (int)TwitchState::Connected) {
        std::lock_guard<std::mutex> lk(twitchMu_);
        settings_.streamKey = twitchKey_;
        settings_.twitchRefreshToken = twitchRefreshNew_;
        settings_.twitchChannel = twitchDisplay_;
        keyText_ = Widen(settings_.streamKey);
        selPlatform_ = 0;  // Twitch
        SaveUiSettings(settings_);
        AddLog(L"Twitch: connected as " + Widen(twitchDisplay_));
        statusText_ = L"Twitch connected — stream key updated";
        statusError_ = false;
    } else if (st == (int)TwitchState::Failed) {
        std::lock_guard<std::mutex> lk(twitchMu_);
        statusText_ = twitchStatus_;
        statusError_ = true;
        AddLog(L"Twitch: " + twitchStatus_);
    } else if (st == (int)TwitchState::WaitingAuth) {
        AddLog(L"Twitch: waiting for the browser confirmation");
    }
    if (twitchOpenActivate_) {
        twitchOpenActivate_ = false;
        std::string uri;
        {
            std::lock_guard<std::mutex> lk(twitchMu_);
            uri = twitchVerifyUri_;
        }
        if (uri.empty()) uri = "https://www.twitch.tv/activate";
        ShellExecuteW(nullptr, L"open", Widen(uri).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// Demo page for the "Test overlay" button: obviously live (moving bar), so one
// click answers "does the browser overlay work?".
static const char* kOverlayTestHtml = R"(<!doctype html><html><head><meta charset="utf-8">
<style>
html,body{margin:0;height:100%;background:transparent;font-family:'Segoe UI',sans-serif}
.box{position:absolute;left:48px;top:48px;padding:20px 26px;border-radius:16px;
background:rgba(22,16,32,.85);border:2px solid #A855F7;color:#fff;
box-shadow:0 10px 40px rgba(0,0,0,.45)}
h1{margin:0 0 8px;font-size:30px}p{margin:0;color:#cbb8ec;font-size:16px;line-height:1.4}
.bar{position:absolute;left:48px;bottom:48px;width:440px;height:16px;border-radius:8px;
background:rgba(255,255,255,.16);overflow:hidden}
.bar i{display:block;height:100%;width:38%;border-radius:8px;background:#A855F7;
animation:slide 2.4s ease-in-out infinite alternate}
@keyframes slide{from{transform:translateX(0)}to{transform:translateX(268px)}}
</style></head><body>
<div class="box"><h1>Ulticast overlay test</h1>
<p>If you can read this and the bar below is moving,<br>
the browser overlay is working.<br>
Replace the URL with your alerts, chat or widget page.</p></div>
<div class="bar"><i></i></div>
</body></html>
)";

void UiApp::EnsureBrowserSource() {
    if (scenes_.empty()) return;
    UiScene& sc = scenes_[activeScene_];
    for (UiSource& s : sc.sources) {
        if (s.type == SessionSourceType::Browser) {
            s.enabled = true;
            return;
        }
    }
    UiSource b;
    b.type = SessionSourceType::Browser;
    b.enabled = true;
    sc.sources.push_back(b);
}

void UiApp::MoveSource(int index, int delta) {
    if (scenes_.empty()) return;
    UiScene& sc = scenes_[activeScene_];
    const int j = index + delta;
    if (index < 0 || index >= (int)sc.sources.size() || j < 0 || j >= (int)sc.sources.size())
        return;
    if (sc.sources[index].type == SessionSourceType::Capture ||
        sc.sources[j].type == SessionSourceType::Capture) {
        statusText_ = L"Capture sources keep their position (remove and re-add to move)";
        statusError_ = false;
        return;
    }
    std::swap(sc.sources[index], sc.sources[j]);
    SyncSettingsFromUi();
    SaveUiSettings(settings_);
    PushSceneSources();
    statusText_ = L"Source order updated";
    statusError_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void UiApp::DrawActionPopup() {
    if (!openAction_) return;
    if (openAction_ == ID_ADD_SOURCE) {
        popup_.items = SourceMenuItems();
    } else if (openAction_ == ID_WINDOW_PICK) {
        popup_.items = windowPickItems_;
    } else {
        popup_.items = propLabels_;
    }
    if (popup_.items.empty()) {
        openAction_ = 0;
        return;
    }
    popup_.rect = actionRect_;
    popup_.sel = nullptr;
    hasPopup_ = true;
    DrawPopup();
}

void UiApp::DrawPreviewStats(const D2D1_RECT_F& r) {
    const float stripH = Px(54);
    D2D1_RECT_F strip =
        D2D1::RectF(r.left + Px(1), r.bottom - stripH, r.right - Px(1), r.bottom - Px(1));
    dc_->FillRectangle(strip, Br(WithAlpha(kBlack, 0.5f)));

    // Timer = the current output's runtime, not the app session age (the idle
    // preview is a session too and must not tick a recording timer).
    const bool timing = stats_.recording || stats_.streaming;
    const double timerSeconds = stats_.recording ? stats_.recSeconds : stats_.strSeconds;
    Text(timing ? FormatTime(timerSeconds)
                : (previewSession_ ? L"PREVIEW" : L"00:00"),
         fmtMono_, D2D1::RectF(strip.left + Px(18), strip.top + Px(4), strip.left + Px(170),
                               strip.top + Px(34)),
         kText);

    double fps = (sessionRunning_ && stats_.elapsed > 0.1) ? stats_.captured / stats_.elapsed : 0.0;
    const double mbps = sessionRunning_ ? stats_.kbps / 1000.0 : 0.0;
    std::wstring line = Format(L"%.0f fps   ·   %.1f Mbps   ·   %s", fps, mbps,
                               Widen(stats_.backend).c_str());
    if (sessionRunning_ && stats_.width)
        line += Format(L"   ·   %ux%u", stats_.width, stats_.height);
    line += Format(L"   ·   %llu dropped",
                   (unsigned long long)(sessionRunning_ ? stats_.dropped : 0));
    Text(line, fmtSmall_,
         D2D1::RectF(strip.right - Px(560), strip.top + Px(10), strip.right - Px(18),
                     strip.top + Px(28)),
         kTextDim, DWRITE_TEXT_ALIGNMENT_TRAILING);

    std::wstring audio;
    if (previewSession_)
        audio = L"Preview — sources appear when you Start";
    else {
        audio = L"no audio";
        if (!stats_.desktopDevice.empty() && stats_.desktopDevice != L"-")
            audio = stats_.desktopDevice;
        if (!stats_.micDevice.empty() && stats_.micDevice != L"-")
            audio += L" + " + stats_.micDevice;
        if (!stats_.webcamName.empty()) audio += L"   ·   cam " + stats_.webcamName;
    }
    dc_->PushAxisAlignedClip(
        D2D1::RectF(strip.left + Px(18), strip.top + Px(34), strip.right - Px(18),
                    strip.bottom - Px(4)),
        D2D1_ANTIALIAS_MODE_ALIASED);
    Text(audio, fmtSmall_,
         D2D1::RectF(strip.left + Px(18), strip.top + Px(33), strip.right - Px(18),
                     strip.bottom - Px(5)),
         WithAlpha(kTextDim, 0.8f));
    dc_->PopAxisAlignedClip();
}

void UiApp::DrawRecordCards() {
    const float rowH = 32, gap = 4, pad = 10, headH = 18, cw = 212;
    const float cy = 362, ch = pad + headH + 6 * rowH + 5 * gap + pad;

    Card(320, cy, 232, ch, L"RECORDING");
    float y = cy + pad + headH + 4;
    D2D1_RECT_F pathR = S(320 + pad, y, cw - 92, rowH);
    Panel(pathR, 8, kPanel, true);
    dc_->PushAxisAlignedClip(pathR, D2D1_ANTIALIAS_MODE_ALIASED);
    Text(ShortPath(WithExtension(settings_.recordPath, selContainer_ == 1 ? "mp4" : "mkv")),
         fmtSmall_, Inset(pathR, 12, 0), kText);
    dc_->PopAxisAlignedClip();
    Button(ID_BROWSE, S(320 + pad + cw - 84, y, 84, rowH), L"Browse", BtnStyle::Ghost);
    y += rowH + gap;
    Combo(ID_COMBO_CODEC, S(320 + pad, y, cw, rowH), codecs_, &selCodec_);
    y += rowH + gap;
    Combo(ID_COMBO_CONTAINER, S(320 + pad, y, cw, rowH), containers_, &selContainer_);
    y += rowH + gap;
    Toggle(ID_TOGGLE_TRACKS, S(320 + pad, y, cw, rowH), L"Separate mic track",
           &separateTracks_);
    y += rowH + gap;
    Text(selContainer_ == 1 ? L"MP4 needs a clean stop" : L"MKV survives crashes",
         fmtSmall_, S(320 + pad, y, cw, rowH), WithAlpha(kTextDim, 0.7f));
    y += rowH + gap;
    Button(ID_OPENFOLDER, S(320 + pad, y, cw, rowH), L"Open Folder", BtnStyle::Ghost);

    Card(564, cy, 232, ch, L"CLIPS");
    y = cy + pad + headH + 4;
    Toggle(ID_TOGGLE_REPLAY, S(564 + pad, y, cw, rowH), L"Replay", &settings_.replay);
    y += rowH + gap;
    Slider(ID_SLIDER_REPLAY, S(564 + pad, y, cw, rowH), 10, 120, &settings_.replaySeconds,
           L"%ds");
    y += rowH + gap;
    Text(L"Clip key", fmtBody_, S(564 + pad, y, 62, rowH), kTextDim);
    Combo(ID_COMBO_HOTKEY, S(564 + pad + 66, y, cw - 66, rowH), hotkeys_, &selHotkey_);
    y += rowH + gap;
    Text(settings_.replay
             ? Format(L"%s saves the last moments",
                      hotkeys_[(selHotkey_ >= 0 && selHotkey_ < (int)hotkeys_.size()) ? selHotkey_
                                                                                      : 3]
                          .c_str())
             : L"Arm the replay buffer above",
         fmtSmall_, S(564 + pad, y, cw, rowH), WithAlpha(kTextDim, 0.7f));
    y += rowH + gap;
    Button(ID_CLIP, S(564 + pad, y, cw, rowH), L"Save Clip", BtnStyle::Ghost,
           stats_.replayArmed);
    y += rowH + gap;
    Text(L"Clips land next to the recording", fmtSmall_, S(564 + pad, y, cw, rowH),
         WithAlpha(kTextDim, 0.7f));

    Card(808, cy, 232, ch, L"ENCODING");
    y = cy + pad + headH + 4;
    Combo(ID_COMBO_RATE, S(808 + pad, y, cw, rowH), rates_, &selRate_);
    y += rowH + gap;
    if (selRate_ == 0)
        Slider(ID_SLIDER_CQP, S(808 + pad, y, cw, rowH), 1, 51, (int*)&settings_.cqp, L"CQ %d");
    else
        Field(ID_FIELD_BITRATE, S(808 + pad, y, cw, rowH), &bitrateText_, false,
              L"Bitrate (kbps)");
    y += rowH + gap;
    Combo(ID_COMBO_PRESET, S(808 + pad, y, cw, rowH), presets_, &selPreset_);
    y += rowH + gap;
    Slider(ID_SLIDER_BFRAMES, S(808 + pad, y, cw, rowH), 0, 4, &settings_.bframes,
           L"B-frames %d");
    y += rowH + gap;
    Toggle(ID_TOGGLE_LOOKAHEAD, S(808 + pad, y, cw, rowH), L"Lookahead", &settings_.lookahead);
    y += rowH + gap;
    Toggle(ID_TOGGLE_MULTIPASS, S(808 + pad, y, cw, rowH), L"Two-pass", &settings_.multipass);
}

void UiApp::DrawStreamCards() {
    const float rowH = 32, gap = 4, pad = 10, headH = 18, cw = 212;
    const float cy = 362, ch = pad + headH + 6 * rowH + 5 * gap + pad;

    Card(320, cy, 232, ch, L"PLATFORM");
    float y = cy + pad + headH + 4;
    Combo(ID_COMBO_PLATFORM, S(320 + pad, y, cw, rowH), platforms_, &selPlatform_);
    y += rowH + gap;
    if (platformKeys_[selPlatform_] == "custom")
        Field(ID_FIELD_URL, S(320 + pad, y, cw, rowH), &urlText_, false, L"rtmp:// ingest URL");
    else
        Field(ID_FIELD_KEY, S(320 + pad, y, cw, rowH), &keyText_, true, L"Stream key");
    y += rowH + gap;
    Slider(ID_SLIDER_RECONNECT, S(320 + pad, y, cw, rowH), 1, 10, &settings_.streamReconnect,
           L"%dx");
    y += rowH + gap;
    Field(ID_FIELD_STREAMBITRATE, S(320 + pad, y, cw, rowH), &streamBitrateText_, false,
          L"Bitrate kbps (CBR)");
    y += rowH + gap;
    Combo(ID_COMBO_STREAMCODEC, S(320 + pad, y, cw, rowH), streamCodecs_, &selStreamCodec_);
    y += rowH + gap;
    Text(L"Press Go Live to start", fmtSmall_, S(320 + pad, y, cw, rowH),
         WithAlpha(kTextDim, 0.7f));

    Card(564, cy, 232, ch, L"STREAM ENCODER");
    y = cy + pad + headH + 4;
    Combo(ID_COMBO_STREAMPRESET, S(564 + pad, y, cw, rowH), presets_, &selStreamPreset_);
    y += rowH + gap;
    Slider(ID_SLIDER_STREAMBFRAMES, S(564 + pad, y, cw, rowH), 0, 4, &settings_.streamBframes,
           L"B-frames %d");
    y += rowH + gap;
    Toggle(ID_TOGGLE_STREAMLOOKAHEAD, S(564 + pad, y, cw, rowH), L"Lookahead",
           &settings_.streamLookahead);
    y += rowH + gap;
    Toggle(ID_TOGGLE_STREAMMULTIPASS, S(564 + pad, y, cw, rowH), L"Two-pass",
           &settings_.streamMultipass);
    y += rowH + gap;
    Text(L"CBR at the bitrate on the left", fmtSmall_, S(564 + pad, y, cw, rowH),
         WithAlpha(kTextDim, 0.7f));
    y += rowH + gap;
    Text(L"Two-pass: slower, better quality", fmtSmall_, S(564 + pad, y, cw, rowH),
         WithAlpha(kTextDim, 0.7f));

    // TWITCH ACCOUNT: connect once, then the stream key keeps itself current.
    Card(808, cy, 232, ch, L"TWITCH ACCOUNT");
    y = cy + pad + headH + 4;
    {
        std::lock_guard<std::mutex> lk(twitchMu_);
        const bool connected = !settings_.twitchChannel.empty();
        std::wstring st = twitchStatus_;
        if (st.empty())
            st = connected ? (L"Connected as " + Widen(settings_.twitchChannel))
                           : L"Not connected — paste a key or connect";
        Text(st, fmtSmall_, S(808 + pad, y, cw, rowH), connected ? kOk : WithAlpha(kTextDim, 0.8f));
    }
    y += rowH + gap;
    {
        const bool connected = !settings_.twitchChannel.empty();
        const int st = twitchState_.load();
        const bool busy = st == (int)TwitchState::Working || st == (int)TwitchState::WaitingAuth;
        Button(ID_TWITCH_CONNECT, S(808 + pad, y, cw, rowH),
               busy ? L"Waiting…" : (connected ? L"Refresh stream key" : L"Connect Twitch"),
               BtnStyle::Primary, !busy);
        y += rowH + gap;
        Button(ID_TWITCH_DISCONNECT, S(808 + pad, y, cw, rowH), L"Disconnect", BtnStyle::Ghost,
               connected && !busy);
        y += rowH + gap;
        Field(ID_FIELD_TWITCHCID, S(808 + pad, y, cw, rowH), &twitchClientIdText_, false,
              L"Client ID (optional)");
        y += rowH + gap;
        Text(L"Connect once — the key stays current", fmtSmall_, S(808 + pad, y, cw, rowH),
             WithAlpha(kTextDim, 0.7f));
        y += rowH + gap;
        Text(L"Key stored DPAPI-encrypted", fmtSmall_, S(808 + pad, y, cw, rowH),
             WithAlpha(kTextDim, 0.7f));
    }
}

void UiApp::DrawSettingsPage() {
    const float rowH = 32, rowGap = 5, cardGap = 12, pad = 10, headH = 18;

    // ---- left column: capture defaults, devices, appearance, browser
    {
        const float x = 20, w = 490, cw = w - 20;
        float cy = 64;

        Card(x, cy, w, pad + headH + 3 * rowH + 2 * rowGap + pad, L"CAPTURE DEFAULTS");
        float y = cy + pad + headH + 4;
        Combo(ID_COMBO_MONITOR, S(x + pad, y, 300, rowH), monitors_, &selMonitor_, selWindow_ == 0);
        Combo(ID_COMBO_FPS, S(x + pad + 318, y, cw - 318, rowH), fpss_, &selFps_);
        y += rowH + rowGap;
        Combo(ID_COMBO_WINDOW, S(x + pad, y, 300, rowH), windows_, &selWindow_);
        Combo(ID_COMBO_BACKEND, S(x + pad + 318, y, cw - 318, rowH), backends_, &selBackend_);
        y += rowH + rowGap;
        Combo(ID_COMBO_GPU, S(x + pad, y, cw, rowH), gpus_, &selGpu_);
        cy += pad + headH + 3 * rowH + 2 * rowGap + pad + cardGap;

        Card(x, cy, w, pad + headH + 4 * rowH + 3 * rowGap + pad, L"CAMERA & AUDIO");
        y = cy + pad + headH + 4;
        Slider(ID_SLIDER_MICGAIN, S(x + pad, y, 226, rowH), 0, 200, &micGainPct_, L"Mic %d%%");
        Slider(ID_SLIDER_DESKGAIN, S(x + pad + 244, y, 226, rowH), 0, 200, &deskGainPct_,
               L"Desk %d%%");
        y += rowH + rowGap;
        Toggle(ID_TOGGLE_CHROMA, S(x + pad, y, 130, rowH), L"Chroma Key", &settings_.chroma);
        Field(ID_FIELD_CHROMA, S(x + pad + 146, y, 160, rowH), &chromaText_, false, L"00FF00");
        y += rowH + rowGap;
        Text(L"Devices are chosen per source — click a source row on the Record/Stream page",
             fmtSmall_, S(x + pad, y, cw, rowH), WithAlpha(kTextDim, 0.7f));
        y += rowH + rowGap;
        Text(L"Mic gate, compressor and monitoring live in Audio Filters", fmtSmall_,
             S(x + pad, y, cw, rowH), WithAlpha(kTextDim, 0.7f));
        cy += pad + headH + 4 * rowH + 3 * rowGap + pad + cardGap;

        // APPEARANCE: accent swatches + light/dark
        Card(x, cy, w, pad + headH + rowH + pad, L"APPEARANCE");
        y = cy + pad + headH + 4;
        const float sw = Px(26), swGap = Px(8);
        float sx = S(x + pad, 0, 0, 0).left;
        const float sy = S(0, y, 0, rowH).top + Px((rowH - 26) / 2);
        for (int i = 0; i < kAccentCount; ++i) {
            D2D1_RECT_F sr = D2D1::RectF(sx, sy, sx + sw, sy + sw);
            hits_.push_back({ID_ACCENT_BASE + i, sr, true});
            D2D1_COLOR_F c = D2D1::ColorF(kAccents[i].rgb);
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((sr.left + sr.right) / 2,
                                                         (sr.top + sr.bottom) / 2),
                                           sw / 2 - Px(1), sw / 2 - Px(1)),
                             Br(c));
            if (settings_.accent == i)
                dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((sr.left + sr.right) / 2,
                                                             (sr.top + sr.bottom) / 2),
                                               sw / 2 + Px(2), sw / 2 + Px(2)),
                                 Br(kText), Px(2));
            else if (hover_ == ID_ACCENT_BASE + i)
                dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((sr.left + sr.right) / 2,
                                                             (sr.top + sr.bottom) / 2),
                                               sw / 2 + Px(1), sw / 2 + Px(1)),
                                 Br(WithAlpha(kTextDim, 0.7f)), Px(1));
            sx += sw + swGap;
        }
        Toggle(ID_TOGGLE_LIGHT, S(x + pad + 292, y, cw - 292, rowH), L"Light mode",
               &settings_.light);
        cy += pad + headH + rowH + pad + cardGap;

        Card(x, cy, w, pad + headH + 2 * rowH + rowGap + pad, L"BROWSER OVERLAY");
        y = cy + pad + headH + 4;
        Field(ID_FIELD_BROWSER, S(x + pad, y, cw, rowH), &browserText_, false,
              L"HTML file or https:// URL (alerts, chat, widgets)");
        y += rowH + rowGap;
        Button(ID_BROWSER_TEST, S(x + pad, y, 150, rowH), L"Test overlay", BtnStyle::Ghost);
        Button(ID_BROWSER_RELOAD, S(x + pad + 158, y, 100, rowH), L"Reload", BtnStyle::Ghost,
               sessionRunning_ && !settings_.browserUrl.empty());
        dc_->PushAxisAlignedClip(S(x + pad + 266, y, cw - 266, rowH),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        Text(browserStatus_.empty() ? L"not started" : browserStatus_, fmtSmall_,
             S(x + pad + 266, y, cw - 266, rowH),
             browserStatus_.rfind(L"load failed", 0) == 0 ? kRec : WithAlpha(kTextDim, 0.75f));
        dc_->PopAxisAlignedClip();
    }

    // ---- right column: audio filters, encoder, behavior, misc
    {
        const float x = 550, w = 490, cw = w - 20;
        float cy = 64;

        Card(x, cy, w, pad + headH + 4 * rowH + 3 * rowGap + pad, L"AUDIO FILTERS");
        float y = cy + pad + headH + 4;
        Toggle(ID_TOGGLE_MICGATE, S(x + pad, y, 130, rowH), L"Noise gate", &settings_.micGate);
        Slider(ID_SLIDER_GATEDB, S(x + pad + 146, y, cw - 146, rowH), -80, -20,
               &settings_.micGateDb, L"%d dB");
        y += rowH + rowGap;
        Toggle(ID_TOGGLE_COMP, S(x + pad, y, 130, rowH), L"Compressor", &settings_.micComp);
        Slider(ID_SLIDER_COMPDB, S(x + pad + 146, y, cw - 146, rowH), -40, -6,
               &settings_.micCompDb, L"%d dB");
        y += rowH + rowGap;
        Slider(ID_SLIDER_COMPRATIO, S(x + pad, y, 226, rowH), 15, 60, &settings_.micCompRatio,
               L"Ratio %.1f:1", 0.1f);
        Text(L"-1 dB safety clip always on", fmtSmall_, S(x + pad + 244, y, cw - 244, rowH),
             WithAlpha(kTextDim, 0.7f));
        y += rowH + rowGap;
        Toggle(ID_TOGGLE_MONITOR, S(x + pad, y, 221, rowH), L"Monitor mic", &settings_.monitorMic);
        Text(L"headphones recommended", fmtSmall_, S(x + pad + 249, y, cw - 249, rowH),
             WithAlpha(kTextDim, 0.7f));
        cy += pad + headH + 4 * rowH + 3 * rowGap + pad + cardGap;

        Card(x, cy, w, pad + headH + 3 * rowH + 2 * rowGap + pad, L"BEHAVIOR");
        y = cy + pad + headH + 4;
        Toggle(ID_TOGGLE_IDLEPREVIEW, S(x + pad, y, 221, rowH), L"Idle preview",
               &settings_.idlePreview);
        Toggle(ID_TOGGLE_TRAYCLOSE, S(x + pad + 249, y, 221, rowH), L"Close to tray",
               &settings_.closeToTray);
        y += rowH + rowGap;
        Toggle(ID_TOGGLE_FADE, S(x + pad, y, 221, rowH), L"Scene fade", &settings_.sceneFade);
        Text(L"250 ms crossfade when switching scenes", fmtSmall_,
             S(x + pad + 249, y, 221, rowH), WithAlpha(kTextDim, 0.7f));
        y += rowH + rowGap;
        Text(L"The camera opens only while recording, streaming or replay is armed", fmtSmall_,
             S(x + pad, y, cw, rowH), WithAlpha(kTextDim, 0.7f));
        cy += pad + headH + 3 * rowH + 2 * rowGap + pad + cardGap;

        Card(x, cy, w, pad + headH + rowH + pad, L"TROUBLESHOOTING");
        y = cy + pad + headH + 4;
        Button(ID_OPENLOGS, S(x + pad, y, 150, rowH), L"Open Logs", BtnStyle::Ghost);
        Button(ID_REPORT, S(x + pad + 158, y, 150, rowH), L"Report Bug", BtnStyle::Ghost);
        Button(ID_OPENFOLDER, S(x + pad + 316, y, 154, rowH), L"Open Folder", BtnStyle::Ghost);
        cy += pad + headH + rowH + pad + cardGap;

        Card(x, cy, w, pad + headH + rowH + pad, L"ABOUT");
        y = cy + pad + headH + 4;
        Text(Format(L"Ulticast %hs  ·  %hs", ULTI_VERSION, ULTI_REPO), fmtSmall_,
             S(x + pad, y, cw, rowH), kTextDim);
        cy += pad + headH + rowH + pad + cardGap;

        // Hints fill the rest of the column (no dead space).
        Text(L"F9 saves a clip  ·  drag the webcam on the preview  ·  click a source row to configure it",
             fmtSmall_, S(x + pad, cy + 6, cw, rowH), WithAlpha(kTextDim, 0.7f));
        Text(L"The camera opens only while recording, streaming or replay is armed.", fmtSmall_,
             S(x + pad, cy + 6 + 20, cw, rowH), WithAlpha(kTextDim, 0.7f));
    }

    DrawActivityStrip(S(20, 606, 1020, 28), 2);
    DrawTransport(2);  // both actions stay reachable from Settings
}

void UiApp::DrawTransport(int mode) {
    const bool rec = stats_.recording;
    const bool live = stats_.streaming;
    const bool active = rec || live;
    float x = 20;
    if (mode != 1) {
        Button(ID_REC, S(x, 640, 176, 44), rec ? L"Stop Recording" : L"Start Recording",
               rec ? BtnStyle::Danger : BtnStyle::Primary);
        x += 184;
    }
    if (mode != 0) {
        Button(ID_LIVE, S(x, 640, 156, 44), live ? L"End Stream" : L"Go Live",
               live ? BtnStyle::Danger : BtnStyle::Primary);
        x += 164;
    }
    Button(ID_PAUSE, S(x, 640, 100, 44), stats_.paused ? L"Resume" : L"Pause", BtnStyle::Ghost,
           active);
    if (mode == 2) {
        Button(ID_CLIP, S(x + 108, 640, 112, 44), L"Snap Clip", BtnStyle::Ghost,
               stats_.replayArmed);
        Button(ID_OPENFOLDER, S(x + 228, 640, 132, 44), L"Open Folder", BtnStyle::Ghost);
    }
    Button(ID_OPENLOGS, S(830, 640, 86, 44), L"Logs", BtnStyle::Ghost);
    Button(ID_REPORT, S(924, 640, 116, 44), L"Report Bug", BtnStyle::Ghost);
}

void UiApp::DrawActivityStrip(const D2D1_RECT_F& r, int maxLines) {
    std::vector<std::wstring> lines;
    {
        std::lock_guard<std::mutex> lock(logMu_);
        for (auto it = logLines_.rbegin(); it != logLines_.rend() && (int)lines.size() < maxLines;
             ++it)
            lines.push_back(*it);
        std::reverse(lines.begin(), lines.end());
    }
    if (!statusText_.empty()) {
        if ((int)lines.size() < maxLines)
            lines.push_back(statusText_);
        else if (!lines.empty())
            lines.back() = statusText_;
    }
    if (lines.empty()) {
        Text(L"Ready", fmtSmall_, Inset(r, 2, 0), WithAlpha(kTextDim, 0.55f));
        return;
    }
    const float lineH = Px(14);
    for (size_t i = 0; i < lines.size(); ++i) {
        D2D1_RECT_F lr = D2D1::RectF(r.left, r.top + lineH * i, r.right, r.top + lineH * (i + 1));
        bool isError = statusError_ && i + 1 == lines.size();
        bool isNewest = i + 1 == lines.size();
        dc_->PushAxisAlignedClip(lr, D2D1_ANTIALIAS_MODE_ALIASED);
        Text(lines[i], fmtSmall_, lr,
             isError ? kRec : (isNewest ? WithAlpha(kText, 0.75f) : WithAlpha(kTextDim, 0.55f)));
        dc_->PopAxisAlignedClip();
    }
}

void UiApp::DrawPopup() {
    if (!hasPopup_) return;
    const float itemH = 34;
    const float visibleH = (std::min)(kWinH, designH_);  // design px actually on screen
    int maxItems = (int)((visibleH - 64) / itemH);
    if (maxItems < 3) maxItems = 3;
    const int total = (int)popup_.items.size();
    const int shown = (std::min)(total, maxItems);
    if (popupScroll_ < 0) popupScroll_ = 0;
    if (popupScroll_ > total - shown) popupScroll_ = total - shown;
    if (popupScroll_ < 0) popupScroll_ = 0;

    float listH = itemH * shown + 8;
    D2D1_RECT_F list = D2D1::RectF(popup_.rect.left, popup_.rect.bottom + Px(4),
                                   popup_.rect.right, popup_.rect.bottom + Px(4) + Px(listH));
    if (list.bottom > Px(visibleH) - Px(12))
        list = D2D1::RectF(popup_.rect.left, popup_.rect.top - Px(4) - Px(listH),
                           popup_.rect.right, popup_.rect.top - Px(4));
    if (list.top < Px(8))
        list = D2D1::RectF(list.left, Px(8), list.right, Px(8) + Px(listH));
    Panel(list, 8, kPanel2, true);

    for (int vi = 0; vi < shown; ++vi) {
        const int i = popupScroll_ + vi;
        D2D1_RECT_F item = D2D1::RectF(list.left + Px(4), list.top + Px(4) + Px(itemH * vi),
                                       list.right - Px(4), list.top + Px(4) + Px(itemH * (vi + 1)));
        hits_.push_back({ID_ITEM_BASE + i, item, true});
        bool hovered = hover_ == ID_ITEM_BASE + i;
        bool selected = popup_.sel && i == *popup_.sel;
        if (hovered || selected)
            dc_->FillRoundedRectangle(D2D1::RoundedRect(item, Px(6), Px(6)),
                                      Br(WithAlpha(kAccent, selected ? 0.35f : 0.2f)));
        dc_->PushAxisAlignedClip(item, D2D1_ANTIALIAS_MODE_ALIASED);
        Text(popup_.items[i], fmtBody_, Inset(item, 10, 0), kText);
        dc_->PopAxisAlignedClip();
    }

    if (total > shown) {
        // scroll hint (mouse wheel)
        D2D1_RECT_F hint = D2D1::RectF(list.left, list.bottom - Px(16), list.right - Px(10),
                                       list.bottom);
        std::wstring s = Format(L"%d / %d  (scroll)", popupScroll_ + shown, total);
        Text(s, fmtSmall_, hint, WithAlpha(kTextDim, 0.8f), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void UiApp::Card(float x, float y, float w, float h, const std::wstring& title) {
    Panel(S(x, y, w, h), 12, WithAlpha(kPanel, 0.5f), true);
    Text(title, fmtSection_, S(x + 14, y + 9, w - 28, 16), WithAlpha(kTextDim, 0.95f));
}

// ----------------------------------------------------------------- preview

void UiApp::RefreshPreview() {
    auto tex = session_.PreviewTexture();
    if (!tex || !sessionRunning_) {
        previewBmp_.Reset();
        previewTex_.Reset();
        lastPreviewTex_.Reset();
        previewW_ = previewH_ = 0;
        previewVer_ = 0;
        return;
    }
    // Re-open the shared handle whenever the underlying texture changes
    // (session restart) -- keeping a reference makes the pointer identity safe.
    if (tex.Get() != lastPreviewTex_.Get()) {
        previewBmp_.Reset();
        previewTex_.Reset();
        previewW_ = previewH_ = 0;
        lastPreviewTex_ = tex;
    }
    uint64_t v = session_.PreviewVersion();
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (previewW_ != d.Width || previewH_ != d.Height) {
        previewBmp_.Reset();
        previewTex_.Reset();
        previewW_ = d.Width;
        previewH_ = d.Height;
        ComPtr<IDXGIResource> res;
        if (SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(res.GetAddressOf())))) {
            HANDLE shared = nullptr;
            if (SUCCEEDED(res->GetSharedHandle(&shared)) && shared) {
                ComPtr<ID3D11Texture2D> opened;
                if (SUCCEEDED(d3d_->OpenSharedResource(shared, __uuidof(ID3D11Texture2D),
                                                       (void**)opened.GetAddressOf())))
                    previewTex_ = opened;
            }
        }
    }
    if (previewTex_ && (!previewBmp_ || v != previewVer_)) {
        if (!previewBmp_) {
            ComPtr<IDXGISurface> surface;
            if (SUCCEEDED(previewTex_.As(&surface))) {
                D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                                 previewBmp_.GetAddressOf());
            }
        }
        previewVer_ = v;
    }
}

// ------------------------------------------------------------------- logic

void UiApp::ReloadDevices() {
    monitors_.clear();
    for (auto& m : WgcCapture::ListMonitors())
        monitors_.push_back(Format(L"Display %d  (%ldx%ld)", m.index + 1, m.rect.right - m.rect.left,
                                   m.rect.bottom - m.rect.top));
    if (monitors_.empty()) monitors_.push_back(L"(no monitors found)");

    windows_.clear();
    windows_.push_back(L"Full monitor");
    for (auto& t : WgcCapture::ListWindows()) windows_.push_back(t);

    webcams_.clear();
    webcams_.push_back(L"Default camera");
    for (auto& d : WebcamCapture::ListDevices()) webcams_.push_back(d);
    // Merge DirectShow-only devices (virtual cameras MF cannot see) so they are
    // selectable; the engine falls back to DirectShow when MF cannot open one.
    for (auto& d : DshowCapture::ListDevices()) {
        bool have = false;
        for (auto& existing : webcams_) {
            std::wstring a = existing, b = d;
            for (auto& c : a) c = (wchar_t)towlower(c);
            for (auto& c : b) c = (wchar_t)towlower(c);
            if (a == b) {
                have = true;
                break;
            }
        }
        if (!have) webcams_.push_back(d);
    }

    mics_.clear();
    mics_.push_back(L"Default microphone");
    for (auto& d : WasapiCapture::ListCaptureDevices()) mics_.push_back(d.name);

    desktops_.clear();
    desktops_.push_back(L"Default output (desktop audio)");
    for (auto& d : WasapiCapture::ListRenderDevices()) desktops_.push_back(d.name);

    gpus_.clear();
    gpuValues_.clear();
    gpuNvenc_.clear();
    gpus_.push_back(L"Auto (prefers NVIDIA)");
    gpuValues_.push_back(-1);
    gpuNvenc_.push_back(true);
    for (auto& a : D3DContext::ListAdapters()) {
        const bool nv = a.vendorId == 0x10DE;
        std::wstring name = a.name;
        if (!nv) name += L"  (no NVENC)";
        gpus_.push_back(name);
        gpuValues_.push_back(a.index);
        gpuNvenc_.push_back(nv);
    }
    rates_ = {L"Quality (CQ)", L"CBR (bitrate)"};
    streamCodecs_ = {L"H.264 (all platforms)", L"HEVC (YouTube etc.)"};
    containers_ = {L"MKV (crash-safe)", L"MP4 (compatible)"};
    hotkeys_ = {L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12", L"Off"};
    hotkeyValues_ = {VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, 0};

    // No implicit game hooking: WGC is the default; DXGI is an explicit choice.
    backends_ = {L"WGC (desktop)", L"DXGI (fullscreen games)"};
    backendKeys_ = {"wgc", "dxgi"};
    fpss_ = {L"30 fps", L"60 fps", L"120 fps"};
    fpsValues_ = {30, 60, 120};
    codecs_ = {L"HEVC (smaller files)", L"H.264 (compatible)"};
    codecHevc_ = {true, false};
    presets_.clear();
    presetValues_.clear();
    for (int p = 1; p <= 7; ++p) {
        presets_.push_back(Format(L"Preset P%d%s", p, p <= 2 ? L" · fast" : (p >= 6 ? L" · best" : L"")));
        presetValues_.push_back(p);
    }
    positions_ = {L"Top left", L"Top right", L"Bottom left", L"Bottom right"};
    posKeys_ = {"topleft", "topright", "bottomleft", "bottomright"};
    platforms_ = {L"Twitch", L"YouTube", L"Facebook", L"Custom URL"};
    platformKeys_ = {"twitch", "youtube", "facebook", "custom"};

    // clamp selections
    selMonitor_ = std::clamp(selMonitor_, 0, (int)monitors_.size() - 1);
    selWindow_ = std::clamp(selWindow_, 0, (int)windows_.size() - 1);
    selWebcam_ = std::clamp(selWebcam_, 0, (int)webcams_.size() - 1);
    selMic_ = std::clamp(selMic_, 0, (int)mics_.size() - 1);
    selBackend_ = std::clamp(selBackend_, 0, (int)backendKeys_.size() - 1);
    selFps_ = std::clamp(selFps_, 0, (int)fpsValues_.size() - 1);
    selCodec_ = std::clamp(selCodec_, 0, (int)codecHevc_.size() - 1);
    selPreset_ = std::clamp(selPreset_, 0, (int)presetValues_.size() - 1);
    selPos_ = std::clamp(selPos_, 0, (int)posKeys_.size() - 1);
    selPlatform_ = std::clamp(selPlatform_, 0, (int)platformKeys_.size() - 1);
}

void UiApp::ApplySettingsToUi() {
    keyText_ = Widen(settings_.streamKey);
    urlText_ = Widen(settings_.customUrl);
    chromaText_ = Widen(settings_.chromaColor);
    browserText_ = Widen(settings_.browserUrl);
    bitrateText_ = Format(L"%u", settings_.recordBitrate);
    streamBitrateText_ = Format(L"%u", settings_.streamBitrate);
    twitchClientIdText_ = Widen(settings_.twitchClientId);
    micGainPct_ = (int)std::lround(settings_.micGain * 100.0f);
    deskGainPct_ = (int)std::lround(settings_.desktopGain * 100.0f);
    separateTracks_ = settings_.audioTracks == 2;
    selRate_ = settings_.recordCbr ? 1 : 0;
    selStreamCodec_ = settings_.streamHevc ? 1 : 0;
    selContainer_ = settings_.container == "mp4" ? 1 : 0;
    for (size_t i = 0; i < hotkeyValues_.size(); ++i)
        if (hotkeyValues_[i] == settings_.hotkey) selHotkey_ = (int)i;
    for (size_t i = 0; i < presetValues_.size(); ++i)
        if (presetValues_[i] == settings_.streamPreset) selStreamPreset_ = (int)i;

    auto find = [](const std::vector<std::wstring>& list, const std::wstring& needle) {
        if (needle.empty()) return 0;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == needle) return (int)i;
        return 0;
    };
    for (size_t i = 0; i < backendKeys_.size(); ++i)
        if (backendKeys_[i] == settings_.backend) selBackend_ = (int)i;
    for (size_t i = 0; i < fpsValues_.size(); ++i)
        if (fpsValues_[i] == settings_.fps) selFps_ = (int)i;
    for (size_t i = 0; i < codecHevc_.size(); ++i)
        if (codecHevc_[i] == settings_.hevc) selCodec_ = (int)i;
    for (size_t i = 0; i < presetValues_.size(); ++i)
        if (presetValues_[i] == settings_.preset) selPreset_ = (int)i;
    for (size_t i = 0; i < posKeys_.size(); ++i)
        if (posKeys_[i] == settings_.webcamPos) selPos_ = (int)i;
    for (size_t i = 0; i < platformKeys_.size(); ++i)
        if (platformKeys_[i] == settings_.platform) selPlatform_ = (int)i;
    selMonitor_ = settings_.monitor;
    selWebcam_ = find(webcams_, Widen(settings_.webcamDevice));
    selMic_ = find(mics_, Widen(settings_.micDevice));
    selDesk_ = find(desktops_, Widen(settings_.desktopDevice));
    for (size_t i = 0; i < gpuValues_.size(); ++i)
        if (gpuValues_[i] == settings_.gpu) selGpu_ = (int)i;
    if (!settings_.windowTitle.empty()) selWindow_ = find(windows_, Widen(settings_.windowTitle));

    // scenes (each with its own source list)
    scenes_.clear();
    for (const UiSceneSetting& s : settings_.scenes) {
        UiScene us;
        us.name = Widen(s.name.empty() ? "Scene" : s.name);
        us.cursor = s.cursor != 0;
        for (const UiSourceSetting& ss : s.sources) {
            UiSource u;
            u.type = (SessionSourceType)std::clamp(ss.type, 0, 4);
            u.enabled = ss.enabled != 0;
            u.device = ss.device;
            u.monitor = ss.monitor;
            u.window = Widen(ss.window);
            u.dxgi = ss.dxgi != 0;
            u.corner = (ss.corner >= 0 && ss.corner < 4) ? ss.corner : 3;
            u.camX = ss.camX;
            u.camY = ss.camY;
            us.sources.push_back(u);
        }
        EnsureSceneDefaults(us);
        scenes_.push_back(us);
    }
    if (scenes_.empty()) {
        UiScene us;
        us.name = L"Game";
        EnsureSceneDefaults(us);
        scenes_.push_back(us);
    }
    activeScene_ = settings_.activeScene;
    if (activeScene_ < 0 || activeScene_ >= (int)scenes_.size()) activeScene_ = 0;
    SyncSceneName();
}

void UiApp::SyncSettingsFromUi() {
    settings_.monitor = selMonitor_;
    settings_.windowTitle = (selWindow_ > 0 && selWindow_ < (int)windows_.size())
                                ? Narrow(windows_[selWindow_])
                                : "";
    settings_.backend = backendKeys_[selBackend_];
    settings_.gpu = (selGpu_ >= 0 && selGpu_ < (int)gpuValues_.size()) ? gpuValues_[selGpu_] : -1;
    settings_.fps = fpsValues_[selFps_];
    settings_.hevc = codecHevc_[selCodec_];
    settings_.preset = presetValues_[selPreset_];
    settings_.recordCbr = selRate_ == 1;
    settings_.recordBitrate = (uint32_t)(std::max)(1000, _wtoi(bitrateText_.c_str()));
    settings_.streamBitrate =
        (uint32_t)std::clamp(_wtoi(streamBitrateText_.c_str()), 100, 200000);
    settings_.streamHevc = selStreamCodec_ == 1;
    settings_.streamPreset = (selStreamPreset_ >= 0 && selStreamPreset_ < (int)presetValues_.size())
                                 ? presetValues_[selStreamPreset_]
                                 : 5;
    settings_.container = selContainer_ == 1 ? "mp4" : "mkv";
    if (selHotkey_ >= 0 && selHotkey_ < (int)hotkeyValues_.size())
        settings_.hotkey = hotkeyValues_[selHotkey_];
    settings_.recordPath = WithExtension(settings_.recordPath, settings_.container);
    settings_.twitchClientId = Narrow(twitchClientIdText_);
    settings_.micGain = micGainPct_ / 100.0f;
    settings_.desktopGain = deskGainPct_ / 100.0f;
    settings_.audioTracks = separateTracks_ ? 2 : 1;
    settings_.desktopDevice = (selDesk_ > 0 && selDesk_ < (int)desktops_.size())
                                  ? Narrow(desktops_[selDesk_])
                                  : "";
    settings_.webcamDevice = (selWebcam_ > 0 && selWebcam_ < (int)webcams_.size())
                                 ? Narrow(webcams_[selWebcam_])
                                 : "";
    settings_.micDevice =
        (selMic_ > 0 && selMic_ < (int)mics_.size()) ? Narrow(mics_[selMic_]) : "";
    settings_.webcamPos = posKeys_[selPos_];
    settings_.platform = platformKeys_[selPlatform_];
    settings_.streamKey = Narrow(keyText_);
    settings_.customUrl = Narrow(urlText_);
    settings_.chromaColor = Narrow(chromaText_);
    settings_.browserUrl = Narrow(browserText_);

    settings_.scenes.clear();
    for (const UiScene& s : scenes_) {
        UiSceneSetting us;
        us.name = Narrow(s.name.empty() ? L"Scene" : s.name);
        us.cursor = s.cursor ? 1 : 0;
        for (const UiSource& src : s.sources) {
            UiSourceSetting ss;
            ss.type = (int)src.type;
            ss.enabled = src.enabled ? 1 : 0;
            ss.device = src.device;
            ss.monitor = src.monitor;
            ss.window = Narrow(src.window);
            ss.dxgi = src.dxgi ? 1 : 0;
            ss.corner = src.corner;
            ss.camX = src.camX;
            ss.camY = src.camY;
            us.sources.push_back(ss);
        }
        settings_.scenes.push_back(us);
    }
    settings_.activeScene = activeScene_;
}

void UiApp::SyncSceneName() {
    if (scenes_.empty()) {
        sceneName_.clear();
        return;
    }
    if (activeScene_ < 0 || activeScene_ >= (int)scenes_.size()) activeScene_ = 0;
    sceneName_ = scenes_[activeScene_].name;
}

bool UiApp::SceneUsesWebcam() const {
    for (const UiScene& s : scenes_)
        for (const UiSource& src : s.sources)
            if (src.type == SessionSourceType::Webcam && src.enabled) return true;
    return false;
}

bool UiApp::SceneUsesBrowser() const {
    for (const UiScene& s : scenes_)
        for (const UiSource& src : s.sources)
            if (src.type == SessionSourceType::Browser && src.enabled) return true;
    return false;
}

std::vector<SessionSource> UiApp::SceneSources(const UiScene& sc) const {
    std::vector<SessionSource> out;
    for (const UiSource& s : sc.sources) {
        SessionSource ss;
        ss.type = s.type;
        ss.enabled = s.enabled;
        ss.device = s.device;
        ss.monitor = s.monitor;
        ss.window = s.window;
        ss.dxgi = s.dxgi;
        ss.corner = s.corner;
        ss.xr = s.camX;
        ss.yr = s.camY;
        ss.audioDevice = Widen(s.device);
        out.push_back(ss);
    }
    return out;
}

void UiApp::EnsureSceneDefaults(UiScene& scene) const {
    bool hasCap = false, hasOut = false;
    for (const UiSource& s : scene.sources) {
        if (s.type == SessionSourceType::Capture) hasCap = true;
        if (s.type == SessionSourceType::AudioOut) hasOut = true;
    }
    if (!hasCap) {
        UiSource c;
        c.type = SessionSourceType::Capture;
        c.enabled = true;
        c.monitor = selMonitor_;
        if (selWindow_ > 0 && selWindow_ < (int)windows_.size()) c.window = windows_[selWindow_];
        if (selBackend_ >= 0 && selBackend_ < (int)backendKeys_.size())
            c.dxgi = backendKeys_[selBackend_] == "dxgi";
        scene.sources.insert(scene.sources.begin(), c);
    }
    if (!hasOut) {
        UiSource a;
        a.type = SessionSourceType::AudioOut;
        a.enabled = settings_.desktopAudio;
        a.device = settings_.desktopDevice;
        scene.sources.push_back(a);
    }
}

void UiApp::PushSceneSources() {
    if (!sessionRunning_ || scenes_.empty()) return;
    const UiScene& sc = scenes_[activeScene_];
    session_.SetSources(SceneSources(sc), "");
    session_.SetCursorVisible(sc.cursor);
    // Audio sources: enable/disable live (device changes apply on next start).
    bool aIn = false, aOut = false;
    for (const UiSource& s : sc.sources) {
        if (!s.enabled) continue;
        if (s.type == SessionSourceType::AudioIn) aIn = true;
        if (s.type == SessionSourceType::AudioOut) aOut = true;
    }
    session_.SetAudioSourcesLive(aOut, aIn);
}

void UiApp::ApplyLiveScene() {
    if (!sessionRunning_ || scenes_.empty()) return;
    PushSceneSources();
    const UiScene& sc = scenes_[activeScene_];
    AddLog(L"Scene: " + sc.name);
    if (previewSession_) return;  // no pipeline hints while previewing

    // Pipelines are decided at Start; tell the user if a newly added source
    // can't appear until the next Start.
    const Session::Stats st = session_.GetStats();
    if (SceneUsesWebcam() && st.webcamName.empty())
        AddLog(L"Video source added — press Stop/Start to switch the device on");
    if (SceneUsesBrowser() && !st.browserActive)
        AddLog(L"Browser source added — press Stop/Start to load it");
}

std::string UiApp::BuildStreamUrl() {
    const std::string key = Narrow(keyText_);
    switch (selPlatform_) {
        case 0:
            return "rtmp://live.twitch.tv/app/" + key;
        case 1:
            return "rtmps://a.rtmps.youtube.com/live2/" + key;
        case 2:
            return "rtmps://live-api-s.facebook.com:443/rtmp/" + key;
        default:
            return Narrow(urlText_);
    }
}

SessionConfig UiApp::BuildConfig() {
    SyncSettingsFromUi();
    SessionConfig cfg;
    const UiScene& sc = scenes_[activeScene_];
    // Capture source (monitor / window / DXGI) comes from the active scene.
    cfg.monitor = 0;
    cfg.window.clear();
    cfg.backend = "wgc";
    for (const UiSource& s : sc.sources) {
        if (s.type != SessionSourceType::Capture || !s.enabled) continue;
        cfg.monitor = s.monitor;
        cfg.window = s.window;
        cfg.backend = s.dxgi ? "dxgi" : "wgc";
        break;
    }
    cfg.adapter = settings_.gpu;
    cfg.fps = settings_.fps;
    cfg.record = settings_.record;
    cfg.recordPath = WithExtension(settings_.recordPath, settings_.container);
    cfg.stream = settings_.stream;
    cfg.streamUrl = BuildStreamUrl();
    cfg.streamBitrate = settings_.streamBitrate;
    cfg.streamHevc = settings_.streamHevc;
    cfg.reconnect = settings_.streamReconnect;
    cfg.hevc = settings_.hevc;
    cfg.cqp = settings_.cqp;
    cfg.recordCbr = settings_.recordCbr;
    cfg.recordBitrate = settings_.recordBitrate;
    cfg.bframes = settings_.bframes;
    cfg.recordLookahead = settings_.lookahead;
    cfg.recordMultipass = settings_.multipass;
    cfg.streamPreset = settings_.streamPreset;
    cfg.streamBframes = settings_.streamBframes;
    cfg.streamLookahead = settings_.streamLookahead;
    cfg.streamMultipass = settings_.streamMultipass;
    cfg.sceneFade = settings_.sceneFade;
    cfg.preset = settings_.preset;
    // Audio sources come from the scene (devices per source).
    cfg.audio.desktop = false;
    cfg.audio.mic = false;
    for (const UiSource& s : sc.sources) {
        if (!s.enabled) continue;
        if (s.type == SessionSourceType::AudioOut) {
            cfg.audio.desktop = true;
            cfg.audio.desktopDevice = s.device.empty() ? L"default" : Widen(s.device);
        } else if (s.type == SessionSourceType::AudioIn) {
            cfg.audio.mic = true;
            cfg.audio.micDevice = s.device.empty() ? L"default" : Widen(s.device);
        }
    }
    cfg.audio.micGain = settings_.micGain;
    cfg.audio.desktopGain = settings_.desktopGain;
    cfg.audio.gate = settings_.micGate;
    cfg.audio.gateDb = (float)settings_.micGateDb;
    cfg.audio.comp = settings_.micComp;
    cfg.audio.compDb = (float)settings_.micCompDb;
    cfg.audio.compRatio = settings_.micCompRatio / 10.0f;
    cfg.audio.monitor = settings_.monitorMic;
    cfg.audio.monitorDevice = L"default";
    cfg.audio.tracks = settings_.audioTracks;
    cfg.audio.tracks = 1;
    cfg.webcamDevice = (selWebcam_ > 0 && selWebcam_ < (int)webcams_.size())
                           ? webcams_[selWebcam_]
                           : L"default";
    // Pipelines start if any scene uses them, so scene switching is instant.
    cfg.webcam = SceneUsesWebcam();
    cfg.browser = SceneUsesBrowser() && !browserText_.empty();
    cfg.browserUrl = Narrow(browserText_);
    cfg.replay = settings_.replay;
    cfg.replaySeconds = settings_.replaySeconds;
    cfg.replayPrefix = settings_.replayPrefix;
    cfg.chroma = settings_.chroma;
    cfg.chromaColor = settings_.chromaColor;
    cfg.chromaLayer = 0;  // UI keys the webcam overlay

    cfg.showCursor = sc.cursor;
    cfg.sources = SceneSources(sc);
    return cfg;
}

void UiApp::AddLog(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(logMu_);
    logLines_.push_back(s);
    if (logLines_.size() > 50) logLines_.pop_front();
}

void UiApp::StartSession(bool record, bool stream) {
    SyncSettingsFromUi();
    SaveUiSettings(settings_);

    if (stream && selPlatform_ != 3 && keyText_.empty()) {
        statusText_ = L"Enter a stream key in Setup to go live";
        statusError_ = true;
        return;
    }
    if (stream && selPlatform_ == 3 && urlText_.empty()) {
        statusText_ = L"Enter the RTMP ingest URL in Setup to go live";
        statusError_ = true;
        return;
    }
    if (settings_.chroma) {
        std::wstring hex = chromaText_;
        bool valid = hex.size() == 6;
        for (wchar_t c : hex)
            if (!iswxdigit(c)) valid = false;
        if (!valid) {
            statusText_ = L"Chroma key color must be 6 hex digits (RRGGBB)";
            statusError_ = true;
            return;
        }
    }
    if (SceneUsesBrowser() && browserText_.empty()) {
        statusText_ = L"Browser overlay needs an HTML file or URL in Setup";
        statusError_ = true;
        return;
    }
    if (selGpu_ > 0 && selGpu_ < (int)gpuNvenc_.size() && !gpuNvenc_[selGpu_]) {
        statusText_ = L"That GPU has no NVENC encoder — pick Auto or your NVIDIA card";
        statusError_ = true;
        return;
    }

    StopPreview();

    SessionConfig cfg = BuildConfig();
    cfg.record = record;
    cfg.stream = stream;
    cfg.replay = settings_.replay;  // replay arms with any session when enabled
    std::string err;
    if (!session_.Start(cfg, &err)) {
        statusText_ = Widen(err);
        statusError_ = true;
        AddLog(L"Start failed: " + statusText_);
    } else {
        statusText_.clear();
        statusError_ = false;
        if (record && stream)
            AddLog(L"Recording + streaming started");
        else if (record)
            AddLog(L"Recording started");
        else if (stream)
            AddLog(L"Streaming started");
        else
            AddLog(L"Replay buffer armed");
    }
}

void UiApp::RecordPressed() {
    if (stats_.recording) {
        session_.DisableRecording();
        if (!stats_.streaming && !stats_.replayArmed) StopSession();
        return;
    }
    if (!sessionRunning_) {
        StartSession(true, false);
        return;
    }
    // A session is already running (streaming/replay): add the file output live.
    session_.EnableRecording(settings_.recordPath);
    statusText_ = L"Recording…";
    statusError_ = false;
}

void UiApp::GoLivePressed() {
    if (stats_.streaming) {
        session_.DisableStreaming();
        if (!stats_.recording && !stats_.replayArmed) StopSession();
        return;
    }
    if (selPlatform_ != 3 && keyText_.empty()) {
        statusText_ = L"Enter a stream key in Setup to go live";
        statusError_ = true;
        return;
    }
    if (selPlatform_ == 3 && urlText_.empty()) {
        statusText_ = L"Enter the RTMP ingest URL in Setup to go live";
        statusError_ = true;
        return;
    }
    if (!sessionRunning_) {
        StartSession(false, true);
        return;
    }
    session_.EnableStreaming(BuildStreamUrl(), settings_.streamBitrate);
    statusText_ = L"Going live…";
    statusError_ = false;
}

void UiApp::StopSession() {
    session_.Stop();
    stats_ = session_.GetStats();
    sessionRunning_ = false;
    previewSession_ = false;
    AddLog(L"Stopped");
    StartPreview();  // back to the idle preview
}

void UiApp::StartPreview() {
    if (!settings_.idlePreview) return;
    if (session_.GetStats().running) return;  // live state, not the cached flag
    SessionConfig cfg = BuildConfig();
    // Preview only: capture + composite + the shared preview texture. No file or
    // stream outputs. The browser overlay runs whenever its source is enabled
    // (it has no exclusive resource); the camera/audio start only when the
    // replay buffer is armed, so they stay free for other apps.
    const bool replayOn = settings_.replay;
    cfg.record = false;
    cfg.stream = false;
    cfg.replay = replayOn;
    cfg.noAudio = !replayOn;
    cfg.audio.desktop = replayOn && settings_.desktopAudio;
    cfg.audio.mic = replayOn && settings_.mic;
    cfg.webcam = replayOn && SceneUsesWebcam();
    cfg.browser = SceneUsesBrowser() && !browserText_.empty();
    if (!cfg.browser) cfg.browserUrl.clear();
    std::string err;
    if (!session_.Start(cfg, &err)) {
        AddLog(L"Preview: " + Widen(err));
        return;
    }
    previewSession_ = true;
    AddLog(L"Idle preview on");
}

void UiApp::StopPreview() {
    if (!previewSession_) return;
    session_.Stop();
    previewSession_ = false;
    sessionRunning_ = false;  // the cached flag must not outlive the session
}

void UiApp::RestartPreview() {
    if (!previewSession_) return;
    StopPreview();
    StartPreview();
}

void UiApp::UpdateWindowTitleState() {
    std::wstring t = L"Ulticast";
    if (stats_.recording && stats_.streaming)
        t += L" — recording + live";
    else if (stats_.recording)
        t += stats_.paused ? L" — paused" : L" — recording";
    else if (stats_.streaming)
        t += L" — live";
    else if (stats_.replayArmed)
        t += L" — replay buffer";
    else if (previewSession_)
        t += L" — preview";
    if (t != lastTitle_) {
        lastTitle_ = t;
        SetWindowTextW(hwnd_, t.c_str());
    }
}

void UiApp::OnAction(int id) {
    if (id >= ID_SCENE_BASE && id < ID_SCENE_BASE + 64) {
        int index = id - ID_SCENE_BASE;
        if (index >= 0 && index < (int)scenes_.size()) {
            activeScene_ = index;
            SyncSceneName();
            ApplyLiveScene();
        }
        openCombo_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id >= ID_SRC_UP_BASE && id < ID_SRC_UP_BASE + 64) {
        MoveSource(id - ID_SRC_UP_BASE, -1);
        return;
    }
    if (id >= ID_SRC_DOWN_BASE && id < ID_SRC_DOWN_BASE + 64) {
        MoveSource(id - ID_SRC_DOWN_BASE, +1);
        return;
    }
    if (id >= ID_ACCENT_BASE && id < ID_ACCENT_BASE + kAccentCount) {
        settings_.accent = id - ID_ACCENT_BASE;
        SyncSettingsFromUi();
        SaveUiSettings(settings_);
        ApplyThemeNow();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id >= ID_SRC_PROP_BASE && id < ID_SRC_PROP_BASE + 64) {
        const int i = id - ID_SRC_PROP_BASE;
        if (i >= 0 && i < (int)scenes_[activeScene_].sources.size()) {
            BuildSourceProperties(i);
            actionSource_ = i;
            openAction_ = ID_SRC_PROP_BASE + i;
            actionRect_ = (i < (int)sourceRowRects_.size()) ? sourceRowRects_[i]
                                                            : D2D1::RectF();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id >= ID_ITEM_BASE && id < ID_ITEM_BASE + 64) {
        int index = id - ID_ITEM_BASE;
        if (openAction_) {
            const int act = openAction_;
            openAction_ = 0;
            if (act == ID_ADD_SOURCE) {
                AddSourceAt(index);
            } else if (act == ID_WINDOW_PICK) {
                if (actionSource_ >= 0 &&
                    actionSource_ < (int)scenes_[activeScene_].sources.size() &&
                    index >= 0 && index < (int)windowPickItems_.size()) {
                    UiSource& s = scenes_[activeScene_].sources[actionSource_];
                    s.window = index == 0 ? std::wstring() : windowPickItems_[index];
                    SyncSettingsFromUi();
                    SaveUiSettings(settings_);
                    PushSceneSources();
                    RestartPreview();
                    if (sessionRunning_ && !previewSession_) {
                        statusText_ = L"Capture source changed — press Stop/Start to apply";
                        statusError_ = false;
                    }
                }
            } else {
                ApplySourceProperty(act - ID_SRC_PROP_BASE, index);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
            if (openCombo_ >= 0 && index >= 0 && index < (int)popup_.items.size()) {
            if (popup_.sel) *popup_.sel = index;
            if (openCombo_ == ID_COMBO_MONITOR || openCombo_ == ID_COMBO_WINDOW ||
                openCombo_ == ID_COMBO_BACKEND || openCombo_ == ID_COMBO_FPS ||
                openCombo_ == ID_COMBO_GPU)
                RestartPreview();
            if (openCombo_ == ID_COMBO_HOTKEY) {
                // Re-register the global clip hotkey with the new choice.
                UnregisterHotKey(hwnd_, 1);
                if (selHotkey_ >= 0 && selHotkey_ < (int)hotkeyValues_.size() &&
                    hotkeyValues_[selHotkey_] != 0)
                    RegisterHotKey(hwnd_, 1, 0, hotkeyValues_[selHotkey_]);
                SyncSettingsFromUi();
                SaveUiSettings(settings_);
                statusText_ = L"Clip hotkey updated";
                statusError_ = false;
            }
            if (openCombo_ == ID_COMBO_CONTAINER || openCombo_ == ID_COMBO_STREAMPRESET) {
                SyncSettingsFromUi();
                SaveUiSettings(settings_);
            }
        }
        openCombo_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    switch (id) {
        case ID_REC:
            RecordPressed();
            break;
        case ID_LIVE:
            GoLivePressed();
            break;
        case ID_PAUSE:
            if (stats_.recording || stats_.streaming)
                session_.SetPaused(!session_.Paused());
            break;
        case ID_CLIP:
            if (stats_.replayArmed)
                session_.SaveClip();
            else {
                statusText_ = L"Replay is off — enable it in Setup > Clips";
                statusError_ = true;
            }
            break;
        case ID_ADD_SOURCE:
            openAction_ = (openAction_ == ID_ADD_SOURCE) ? 0 : ID_ADD_SOURCE;
            actionRect_ = addMenuRect_;
            break;
        case ID_BROWSER_TEST: {
            const std::string path = Diag::DataDir() + "\\overlay-test.html";
            std::ofstream f(path);
            if (f) {
                f << kOverlayTestHtml;
                f.close();
                browserText_ = Widen(path);
                EnsureBrowserSource();
                SyncSettingsFromUi();
                SaveUiSettings(settings_);
                PushSceneSources();
                RestartPreview();
                statusText_ = L"Test overlay added — it should appear in the preview";
                statusError_ = false;
            } else {
                statusText_ = L"Could not write the test page";
                statusError_ = true;
            }
            break;
        }
        case ID_BROWSER_RELOAD:
            session_.ReloadBrowser();
            statusText_ = L"Overlay reloaded";
            statusError_ = false;
            break;
        case ID_TWITCH_CONNECT:
            StartTwitchConnect();
            break;
        case ID_TWITCH_DISCONNECT:
            settings_.twitchRefreshToken.clear();
            settings_.twitchChannel.clear();
            SaveUiSettings(settings_);
            {
                std::lock_guard<std::mutex> lk(twitchMu_);
                twitchStatus_.clear();
            }
            twitchState_.store((int)TwitchState::Idle);
            lastTwitchState_ = (int)TwitchState::Idle;
            statusText_ = L"Twitch disconnected";
            statusError_ = false;
            break;
        case ID_BROWSE: {            wchar_t file[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, settings_.recordPath.c_str(), -1, file, MAX_PATH);
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"Video files\0*.mkv;*.mp4\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"mkv";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn)) settings_.recordPath = Narrow(file);
            break;
        }
        case ID_OPENFOLDER: {
            std::string dir = settings_.recordPath;
            size_t slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            ShellExecuteW(nullptr, L"open", Widen(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_OPENLOGS:
            ShellExecuteW(nullptr, L"open", Widen(Diag::LogDir()).c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
            break;
        case ID_REPORT: {
            // Collect a redacted report and hand the user the issue page.
            std::string report = Diag::BuildReport();
            std::string path = Diag::DataDir() + "\\diagnostics.txt";
            std::ofstream f(path);
            if (f) f << report;
            AddLog(L"Diagnostics saved: " + Widen(path));
            ShellExecuteW(nullptr, L"open", Widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ShellExecuteW(nullptr, L"open", L"" ULTI_REPO "/issues/new?template=bug_report.yml",
                          nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_TOGGLE_REPLAY:
            // The Replay toggle arms/disarms the rolling buffer live. Arming it
            // with no session starts one (replay is an output: the scene needs
            // the camera/browser pipelines running).
            if (settings_.replay) {
                if (!sessionRunning_)
                    StartSession(false, false);
                else
                    session_.EnableReplay(settings_.replaySeconds, settings_.replayPrefix);
                statusText_ = L"Replay buffer armed";
                statusError_ = false;
            } else {
                session_.DisableReplay();
                if (sessionRunning_ && !stats_.recording && !stats_.streaming) StopSession();
                statusText_ = L"Replay buffer off";
                statusError_ = false;
            }
            break;
        case ID_TOGGLE_DESKTOP:
        case ID_TOGGLE_MIC:
            // value already toggled by the widget handler
            break;
        case ID_TOGGLE_IDLEPREVIEW:
            if (settings_.idlePreview) {
                if (!sessionRunning_) StartPreview();
            } else {
                StopPreview();
            }
            break;
        case ID_TOGGLE_LIGHT:
            SyncSettingsFromUi();
            SaveUiSettings(settings_);
            ApplyThemeNow();
            break;
        case ID_TAB_LIVE:
        case ID_TAB_RECORD:
            page_ = 0;
            break;
        case ID_TAB_SETUP:
        case ID_TAB_SETTINGS:
            page_ = 2;
            break;
        case ID_TAB_STREAM:
            page_ = 1;
            break;
        case ID_SCENE_ADD:
            if (scenes_.size() < 6) {
                UiScene s = scenes_[activeScene_];
                s.name = L"Scene " + std::to_wstring(scenes_.size() + 1);
                scenes_.push_back(s);
                activeScene_ = (int)scenes_.size() - 1;
                SyncSceneName();
                ApplyLiveScene();
            }
            break;
        case ID_SCENE_REMOVE:
            if (scenes_.size() > 1) {
                scenes_.erase(scenes_.begin() + activeScene_);
                if (activeScene_ >= (int)scenes_.size())
                    activeScene_ = (int)scenes_.size() - 1;
                SyncSceneName();
                ApplyLiveScene();
            }
            break;
        case ID_SRC_CAPTURE:
        case ID_SRC_WEBCAM:
        case ID_SRC_BROWSER:
        case ID_SRC_CURSOR:
            // Live changes apply immediately when running.
            if (sessionRunning_) ApplyLiveScene();
            break;
        default:
            break;
    }
    UpdateWindowTitleState();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// -------------------------------------------------------------------- tray

void UiApp::InitTray() {
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAY;
    nid_.hIcon = icon_;
    wcscpy_s(nid_.szTip, L"Ulticast");
    Shell_NotifyIconW(NIM_ADD, &nid_);
}

void UiApp::ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    const bool active = stats_.recording || stats_.streaming;
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show Ulticast");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_REC,
                stats_.recording ? L"Stop Recording" : L"Start Recording");
    AppendMenuW(menu, MF_STRING, ID_TRAY_LIVE, stats_.streaming ? L"End Stream" : L"Go Live");
    AppendMenuW(menu, MF_STRING | (!active ? MF_GRAYED : 0), ID_TRAY_PAUSE,
                stats_.paused ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_STRING | (!stats_.replayArmed ? MF_GRAYED : 0), ID_TRAY_CLIP,
                L"Snap Clip");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void UiApp::OnTrayCommand(int id) {
    switch (id) {
        case ID_TRAY_SHOW:
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
            break;
        case ID_TRAY_REC:
            RecordPressed();
            break;
        case ID_TRAY_LIVE:
            GoLivePressed();
            break;
        case ID_TRAY_PAUSE:
            if (stats_.recording || stats_.streaming) session_.SetPaused(!session_.Paused());
            break;
        case ID_TRAY_CLIP:
            if (stats_.replayArmed) session_.SaveClip();
            break;
        case ID_TRAY_EXIT:
            exiting_ = true;
            DestroyWindow(hwnd_);
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ------------------------------------------------------------------ events

LRESULT CALLBACK UiApp::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* self = reinterpret_cast<UiApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<UiApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(hwnd, msg, wp, lp);
}

LRESULT UiApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            ResizeSwapChain(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            scale_ = dpi_ / 96.0f;
            CreateTextFormats();
            auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                bool hand = false, ibeam = false;
                for (auto& h : hits_) {
                    if (h.enabled && h.id == hover_) {
                        if (h.id == ID_FIELD_KEY || h.id == ID_FIELD_URL ||
                            h.id == ID_FIELD_CHROMA || h.id == ID_FIELD_BROWSER ||
                            h.id == ID_SCENE_NAME || h.id == ID_FIELD_BITRATE ||
                            h.id == ID_FIELD_STREAMBITRATE)
                            ibeam = true;
                        else
                            hand = true;
                        break;
                    }
                }
                SetCursor(LoadCursor(nullptr, ibeam ? IDC_IBEAM : (hand ? IDC_HAND : IDC_ARROW)));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            if (hasPopup_ && popup_.items.size() > 1) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                popupScroll_ -= (delta > 0 ? 1 : -1) * 3;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const float s = (dpi_ ? dpi_ : 96) / 96.0f * 0.62f;  // smallest zoom-to-fit size
            mmi->ptMinTrackSize.x = (LONG)(kWinW * s);
            mmi->ptMinTrackSize.y = (LONG)(kWinH * s);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerId) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
            int newHover = 0;
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
                if (it->enabled && x >= it->rect.left && x < it->rect.right && y >= it->rect.top &&
                    y < it->rect.bottom) {
                    newHover = it->id;
                    break;
                }
            }
            if (newHover != hover_) {
                hover_ = newHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (dragging_) {
                auto it = sliderRects_.find(dragging_);
                if (it != sliderRects_.end()) {
                    SetSliderFromX(dragging_, x);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            if (draggingWebcam_ && !scenes_.empty() && stats_.width && previewW_ && previewH_) {
                D2D1_RECT_F pr = PreviewRect();
                const float sc = (std::min)((pr.right - pr.left) / (float)previewW_,
                                            (pr.bottom - pr.top) / (float)previewH_);
                const float ox = (pr.left + pr.right) / 2 - previewW_ * sc / 2;
                const float oy = (pr.top + pr.bottom) / 2 - previewH_ * sc / 2;
                const float canvasW = (float)stats_.width, canvasH = (float)stats_.height;
                const float camW = canvasW / 4.0f, camH = camW * 3.0f / 4.0f;
                float cx = ((x - dragOffX_) - ox) / sc;
                float cy = ((y - dragOffY_) - oy) / sc;
                cx = std::clamp(cx, 0.f, (std::max)(0.f, canvasW - camW));
                cy = std::clamp(cy, 0.f, (std::max)(0.f, canvasH - camH));
                const int vi = FirstVideoSourceIndex();
                if (vi >= 0) {
                    scenes_[activeScene_].sources[vi].camX = cx / canvasW;
                    scenes_[activeScene_].sources[vi].camY = cy / canvasH;
                    PushSceneSources();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            // dragging inside a focused text field extends the selection
            if (press_ != 0 && press_ == focus_) {
                auto it = fieldRects_.find(press_);
                if (it != fieldRects_.end()) {
                    if (std::wstring* t = FieldTarget(press_)) {
                        if (edit_.text != t) EditBind(t);
                        const float originX = it->second.left + Px(12) - edit_.scroll;
                        EditMoveTo(EditCaretIndexFromX(originX, x), true);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
            press_ = 0;
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
                if (it->enabled && x >= it->rect.left && x < it->rect.right && y >= it->rect.top &&
                    y < it->rect.bottom) {
                    press_ = it->id;
                    break;
                }
            }
            // A click that is not on the open popup (its items are hits) or on the
            // popup's own button closes it -- no more "stuck" menus. Escape too.
            const bool onPopupItem = press_ >= ID_ITEM_BASE && press_ < ID_ITEM_BASE + 64;
            const bool onPopupOwner =
                (press_ != 0 && press_ == openCombo_) ||
                (press_ == ID_ADD_SOURCE && openAction_ == ID_ADD_SOURCE);
            if ((openAction_ || openCombo_ >= 0) && !onPopupItem && !onPopupOwner) {
                openAction_ = 0;
                openCombo_ = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (press_ == ID_SLIDER_CQP || press_ == ID_SLIDER_REPLAY ||
                press_ == ID_SLIDER_BFRAMES || press_ == ID_SLIDER_MICGAIN ||
                press_ == ID_SLIDER_DESKGAIN || press_ == ID_SLIDER_RECONNECT ||
                press_ == ID_SLIDER_GATEDB || press_ == ID_SLIDER_COMPDB ||
                press_ == ID_SLIDER_COMPRATIO || press_ == ID_SLIDER_STREAMBFRAMES) {
                dragging_ = press_;
                SetSliderFromX(press_, x);
            }
            if (press_ == ID_PREVIEW_WEBCAM) {
                draggingWebcam_ = true;
                D2D1_RECT_F wr = PreviewWebcamRect(PreviewRect());
                dragOffX_ = x - wr.left;
                dragOffY_ = y - wr.top;
            }
            SetCapture(hwnd_);

            // text fields: focus + caret placement (double-click selects a word)
            int newFocus = 0;
            for (int fid : {ID_FIELD_KEY, ID_FIELD_URL, ID_FIELD_CHROMA, ID_FIELD_BROWSER,
                            ID_SCENE_NAME, ID_FIELD_BITRATE, ID_FIELD_STREAMBITRATE,
                            ID_FIELD_TWITCHCID}) {
                auto it = fieldRects_.find(fid);
                if (it == fieldRects_.end()) continue;
                if (x < it->second.left || x >= it->second.right || y < it->second.top ||
                    y >= it->second.bottom)
                    continue;
                newFocus = fid;
                if (std::wstring* t = FieldTarget(fid)) {
                    EditBind(t);
                    const float originX = it->second.left + Px(12) - edit_.scroll;
                    const int idx = EditCaretIndexFromX(originX, x);
                    const uint64_t now = GetTickCount64();
                    if (edit_.lastClickField == fid && now - edit_.lastClickMs < 400)
                        EditSelectWordAt(idx);  // double click
                    else
                        EditMoveTo(idx, false);
                    edit_.lastClickField = fid;
                    edit_.lastClickMs = now;
                }
            }
            if (newFocus != focus_) {
                focus_ = newFocus;
                if (newFocus) edit_.scroll = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            // toggles act immediately
            for (int tid : {ID_TOGGLE_REPLAY, ID_TOGGLE_DESKTOP, ID_TOGGLE_MIC,
                            ID_TOGGLE_CHROMA, ID_TOGGLE_LOOKAHEAD, ID_TOGGLE_MULTIPASS,
                            ID_TOGGLE_IDLEPREVIEW, ID_TOGGLE_TRAYCLOSE, ID_TOGGLE_MICGATE,
                            ID_TOGGLE_COMP, ID_TOGGLE_LIGHT, ID_TOGGLE_MONITOR, ID_TOGGLE_TRACKS,
                            ID_TOGGLE_FADE, ID_TOGGLE_STREAMLOOKAHEAD, ID_TOGGLE_STREAMMULTIPASS,
                            ID_SRC_CURSOR}) {
                if (press_ == tid) {
                    bool* target = nullptr;
                    switch (tid) {
                        case ID_TOGGLE_REPLAY: target = &settings_.replay; break;
                        case ID_TOGGLE_DESKTOP: target = &settings_.desktopAudio; break;
                        case ID_TOGGLE_MIC: target = &settings_.mic; break;
                        case ID_TOGGLE_CHROMA: target = &settings_.chroma; break;
                        case ID_TOGGLE_LOOKAHEAD: target = &settings_.lookahead; break;
                        case ID_TOGGLE_MULTIPASS: target = &settings_.multipass; break;
                        case ID_TOGGLE_IDLEPREVIEW: target = &settings_.idlePreview; break;
                        case ID_TOGGLE_TRAYCLOSE: target = &settings_.closeToTray; break;
                        case ID_TOGGLE_MICGATE: target = &settings_.micGate; break;
                        case ID_TOGGLE_COMP: target = &settings_.micComp; break;
                        case ID_TOGGLE_MONITOR: target = &settings_.monitorMic; break;
                        case ID_TOGGLE_LIGHT: target = &settings_.light; break;
                        case ID_TOGGLE_TRACKS: target = &separateTracks_; break;
                        case ID_TOGGLE_FADE: target = &settings_.sceneFade; break;
                        case ID_TOGGLE_STREAMLOOKAHEAD: target = &settings_.streamLookahead; break;
                        case ID_TOGGLE_STREAMMULTIPASS: target = &settings_.streamMultipass; break;
                        case ID_SRC_CURSOR: target = &scenes_[activeScene_].cursor; break;
                    }
                    if (target) *target = !*target;
                    SyncSettingsFromUi();
                    SaveUiSettings(settings_);
                    OnAction(tid);
                    press_ = 0;
                }
            }

            // scene source rows: enable/disable applies immediately
            if (press_ >= ID_SRC_TOGGLE_BASE && press_ < ID_SRC_TOGGLE_BASE + 64) {
                const int i = press_ - ID_SRC_TOGGLE_BASE;
                UiScene& sc = scenes_[activeScene_];
                if (i >= 0 && i < (int)sc.sources.size()) {
                    sc.sources[i].enabled = !sc.sources[i].enabled;
                    PushSceneSources();
                    if (sc.sources[i].enabled && sc.sources[i].type == SessionSourceType::Webcam &&
                        sessionRunning_ && !previewSession_) {
                        statusText_ =
                            L"Video source on — press Stop/Start to switch the device on";
                        statusError_ = false;
                    }
                }
                press_ = 0;
            }

            // combos open/close
            for (auto& [id, rect] : comboRects_) {
                if (press_ == id) {
                    openCombo_ = (openCombo_ == id) ? -1 : id;
                    popupScroll_ = 0;
                    if (openCombo_ == ID_COMBO_WINDOW) {
                        // refresh the window list on open
                        std::wstring keep = selWindow_ > 0 && selWindow_ < (int)windows_.size()
                                                ? windows_[selWindow_]
                                                : L"";
                        windows_.clear();
                        windows_.push_back(L"Full monitor");
                        for (auto& t : WgcCapture::ListWindows()) windows_.push_back(t);
                        selWindow_ = 0;
                        for (size_t i = 0; i < windows_.size(); ++i)
                            if (windows_[i] == keep) selWindow_ = (int)i;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
            dragging_ = 0;
            if (draggingWebcam_) {
                draggingWebcam_ = false;
                SyncSettingsFromUi();
                SaveUiSettings(settings_);
            }
            int released = 0;
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
                if (it->enabled && x >= it->rect.left && x < it->rect.right && y >= it->rect.top &&
                    y < it->rect.bottom) {
                    released = it->id;
                    break;
                }
            }
            // ReleaseCapture() delivers WM_CAPTURECHANGED synchronously, which clears
            // press_ — so save it first and match against the saved value.
            const int pressed = press_;
            if (GetCapture() == hwnd_) ReleaseCapture();
            if (released && released == pressed) OnAction(released);
            else if (openCombo_ >= 0 && released != openCombo_ &&
                     !(released >= ID_ITEM_BASE && released < ID_ITEM_BASE + 64)) {
                openCombo_ = -1;
            }
            press_ = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd_) {
                press_ = 0;
                dragging_ = 0;
            }
            return 0;
        case WM_CHAR: {
            wchar_t c = (wchar_t)wp;
            std::wstring* target = FieldTarget(focus_);
            if (!target) return 0;
            if (edit_.text != target) EditBind(target);
            if (c == 1)
                EditSelectAll();      // Ctrl+A
            else if (c == 3)
                EditCopy(false);      // Ctrl+C
            else if (c == 24)
                EditCopy(true);       // Ctrl+X
            else if (c == 22)
                EditPaste();          // Ctrl+V
            else if (c >= 32)
                EditInsert(c);
            else
                return 0;             // backspace/delete arrive as WM_KEYDOWN
            AfterFieldEdit();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                openAction_ = 0;
                openCombo_ = -1;
                focus_ = 0;
                edit_.text = nullptr;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            std::wstring* target = FieldTarget(focus_);
            if (!target) return 0;
            if (edit_.text != target) EditBind(target);
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            switch (wp) {
                case VK_LEFT:
                    if (ctrl) EditWordMove(-1, shift);
                    else EditMove(-1, shift);
                    break;
                case VK_RIGHT:
                    if (ctrl) EditWordMove(+1, shift);
                    else EditMove(+1, shift);
                    break;
                case VK_HOME: EditHome(shift); break;
                case VK_END: EditEnd(shift); break;
                case VK_DELETE: EditDeleteForward(); break;
                case VK_BACK: EditDeleteBack(); break;
                default: return 0;
            }
            AfterFieldEdit();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_HOTKEY:
            if (wp == 1) session_.SaveClip();
            return 0;
        case WM_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowTrayMenu();
            else if (lp == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            }
            return 0;
        case WM_COMMAND:
            if (wp >= ID_TRAY_SHOW && wp <= ID_TRAY_EXIT + 200) OnTrayCommand((int)wp);
            return 0;
        case WM_CLOSE:
            // X closes the app (as expected); enable "Close to tray" in Setup to
            // keep recording in the background instead.
            if (exiting_ || !settings_.closeToTray) {
                exiting_ = true;
                DestroyWindow(hwnd);
            } else {
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int RunUiApp(HINSTANCE instance) {
    UiApp app;
    if (!app.Init(instance)) return 1;
    int rc = app.Run();
    // Stop the session (and close its browser) before shutting CEF down.
    app.Shutdown();
    BrowserOverlay::ShutdownGlobal();
    return rc;
}
