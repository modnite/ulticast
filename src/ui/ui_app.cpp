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
#include "../core/capture_wgc.hpp"
#include "../session.hpp"

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------- constants

constexpr wchar_t kClassName[] = L"UlticastUiWindow";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;

constexpr float kWinW = 1020.f;
constexpr float kWinH = 800.f;

// palette
const D2D1_COLOR_F kBg = D2D1::ColorF(0x0E1014);
const D2D1_COLOR_F kPanel = D2D1::ColorF(0x151922);
const D2D1_COLOR_F kPanel2 = D2D1::ColorF(0x1B2130);
const D2D1_COLOR_F kBorder = D2D1::ColorF(0x273040);
const D2D1_COLOR_F kText = D2D1::ColorF(0xE9EDF5);
const D2D1_COLOR_F kTextDim = D2D1::ColorF(0x8C96A8);
const D2D1_COLOR_F kAccent = D2D1::ColorF(0x7C5CFF);
const D2D1_COLOR_F kAccent2 = D2D1::ColorF(0x22D3EE);
const D2D1_COLOR_F kRec = D2D1::ColorF(0xFF4D6D);
const D2D1_COLOR_F kOk = D2D1::ColorF(0x34D399);
const D2D1_COLOR_F kWarn = D2D1::ColorF(0xFBBF24);
const D2D1_COLOR_F kWhite = D2D1::ColorF(0xFFFFFF);
const D2D1_COLOR_F kBlack = D2D1::ColorF(0x000000);

enum {
    ID_START = 1,
    ID_PAUSE,
    ID_CLIP,
    ID_OPENFOLDER,
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
    ID_FIELD_KEY,
    ID_FIELD_URL,
    ID_FIELD_CHROMA,
    ID_FIELD_BROWSER,
    ID_TOGGLE_CHROMA,
    ID_TOGGLE_BROWSER,
    ID_OPENLOGS,
    ID_REPORT,
    ID_BROWSE,
    ID_ITEM_BASE = 1000,
};

enum {
    ID_TRAY_SHOW = 9001,
    ID_TRAY_START,
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

std::wstring FormatTime(double seconds) {
    int total = (int)seconds;
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

    // scale helpers
    D2D1_RECT_F S(float x, float y, float w, float h) const {
        return D2D1::RectF(x * scale_, y * scale_, (x + w) * scale_, (y + h) * scale_);
    }
    float Px(float v) const { return v * scale_; }
    D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy) const {
        return D2D1::RectF(r.left + dx * scale_, r.top + dy * scale_, r.right - dx * scale_,
                           r.bottom - dy * scale_);
    }

    // drawing
    void Paint();
    void DrawHeader();
    void DrawPreview();
    void DrawStats();
    void DrawLeftControls();
    void DrawRightControls();
    void DrawTransport();
    void DrawActivityStrip();
    void DrawPopup();

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
                bool enabled = true);
    void Combo(int id, const D2D1_RECT_F& r, const std::vector<std::wstring>& items, int* sel,
               bool enabled = true);
    void Slider(int id, const D2D1_RECT_F& r, int minV, int maxV, int* value,
                const wchar_t* suffixFmt);
    void SetSliderFromX(int id, float x);
    void Field(int id, const D2D1_RECT_F& r, std::wstring* text, bool password,
               const wchar_t* placeholder);
    void SectionLabel(const std::wstring& s, float x, float y);
    void Pill(const D2D1_RECT_F& r, const D2D1_COLOR_F& color, const std::wstring& label);

    // data + state
    void ReloadDevices();
    void RefreshPreview();
    void SyncSettingsFromUi();
    void ApplySettingsToUi();
    SessionConfig BuildConfig();
    std::string BuildStreamUrl();
    void StartSession();
    void StopSession();
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
    struct Popup {
        D2D1_RECT_F rect{};
        std::vector<std::wstring> items;
        int* sel = nullptr;
    };
    bool hasPopup_ = false;
    Popup popup_;

    // data
    std::vector<std::wstring> monitors_, windows_, webcams_, mics_, backends_, fpss_, codecs_,
        presets_, positions_, platforms_;
    std::vector<std::string> backendKeys_, platformKeys_, posKeys_;
    std::vector<uint32_t> fpsValues_;
    std::vector<int> presetValues_;
    std::vector<bool> codecHevc_;
    int selMonitor_ = 0, selWindow_ = 0, selWebcam_ = 0, selMic_ = 0, selBackend_ = 0, selFps_ = 1,
        selCodec_ = 0, selPreset_ = 4, selPos_ = 3, selPlatform_ = 0;

    // session + settings
    UiSettings settings_;
    Session session_;
    Session::Stats stats_;
    bool sessionRunning_ = false;
    bool errorShown_ = false;
    std::wstring statusText_;
    bool statusError_ = false;
    std::wstring keyText_, urlText_, chromaText_, browserText_;

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

    RECT r{0, 0, (LONG)kWinW, (LONG)kWinH};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE, 0);

    hwnd_ = CreateWindowExW(0, kClassName, L"Ulticast",
                            WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    // Match the frame to the dark UI: immersive dark caption + rounded corners.
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    if (!InitGraphics()) {
        MessageBoxW(hwnd_, L"Direct2D initialization failed.", L"Ulticast", MB_ICONERROR);
        return false;
    }

    LoadUiSettings(&settings_);
    if (settings_.recordPath.empty()) settings_.recordPath = DefaultRecordingPath();
    ReloadDevices();
    ApplySettingsToUi();
    InitTray();

    session_.SetLogSink([this](const std::string& msg) {
        std::lock_guard<std::mutex> lock(logMu_);
        logLines_.push_back(Widen(msg));
        if (logLines_.size() > 50) logLines_.pop_front();
    });

    if (settings_.hotkey != 0) RegisterHotKey(hwnd_, 1, 0, settings_.hotkey);
    SetTimer(hwnd_, kTimerId, 66, nullptr);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void UiApp::Shutdown() {
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

    if (FAILED(dc_->CreateLayer(nullptr, layer_.GetAddressOf()))) return false;

    // Primary-button gradient (violet, slightly deeper at the bottom edge).
    D2D1_GRADIENT_STOP stops[2] = {};
    stops[0].position = 0.0f;
    stops[0].color = D2D1::ColorF(0x8B6CFF);
    stops[1].position = 1.0f;
    stops[1].color = D2D1::ColorF(0x6B4AF0);
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
    auto rr = D2D1::RoundedRect(r, Px(8), Px(8));

    if (style == BtnStyle::Primary && enabled && gradPrimary_) {
        gradPrimary_->SetStartPoint(D2D1::Point2F(r.left, r.top));
        gradPrimary_->SetEndPoint(D2D1::Point2F(r.left, r.bottom));
        dc_->FillRoundedRectangle(rr, gradPrimary_.Get());
    } else if (style == BtnStyle::Danger && enabled) {
        dc_->FillRoundedRectangle(rr, Br(kRec));
    } else {
        dc_->FillRoundedRectangle(rr, Br(enabled ? kPanel : WithAlpha(kPanel, 0.5f)));
        D2D1_COLOR_F border = !enabled ? WithAlpha(kBorder, 0.45f)
                                       : (hover ? WithAlpha(kAccent, 0.55f) : kBorder);
        dc_->DrawRoundedRectangle(rr, Br(border), Px(1));
    }
    if (hover) dc_->FillRoundedRectangle(rr, Br(WithAlpha(kWhite, 0.06f)));
    if (down) dc_->FillRoundedRectangle(rr, Br(WithAlpha(kBlack, 0.16f)));

    D2D1_COLOR_F fg = (style == BtnStyle::Ghost)
                          ? (enabled ? kText : WithAlpha(kTextDim, 0.55f))
                          : WithAlpha(kWhite, enabled ? 1.0f : 0.5f);
    Text(label, fmtBodyBold_, r, fg, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void UiApp::Toggle(int id, const D2D1_RECT_F& r, const std::wstring& label, bool* value,
                   bool enabled) {
    hits_.push_back({id, r, enabled});
    const float trackW = Px(38), trackH = Px(20);
    D2D1_RECT_F track = D2D1::RectF(r.right - trackW, (r.top + r.bottom) / 2 - trackH / 2, r.right,
                                    (r.top + r.bottom) / 2 + trackH / 2);
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
    const float trackR = it->second.right - Px(64);  // must match Slider()'s track end
    float t = (x - it->second.left) / (trackR - it->second.left);
    t = std::clamp(t, 0.f, 1.f);
    if (id == ID_SLIDER_CQP)
        settings_.cqp = (uint32_t)(16 + t * 14 + 0.5f);
    else if (id == ID_SLIDER_REPLAY)
        settings_.replaySeconds = (int)(10 + t * 110 + 0.5f);
}

void UiApp::Slider(int id, const D2D1_RECT_F& r, int minV, int maxV, int* value,
                   const wchar_t* suffixFmt) {
    hits_.push_back({id, r, true});
    sliderRects_[id] = r;
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
    Text(Format(suffixFmt, *value), fmtSmall_,
         D2D1::RectF(trackR + Px(8), r.top, r.right, r.bottom), kTextDim,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void UiApp::Field(int id, const D2D1_RECT_F& r, std::wstring* text, bool password,
                  const wchar_t* placeholder) {
    hits_.push_back({id, r, true});
    fieldRects_[id] = r;
    bool focused = focus_ == id;
    auto rr = D2D1::RoundedRect(r, Px(8), Px(8));
    dc_->FillRoundedRectangle(rr, Br(kPanel));
    dc_->DrawRoundedRectangle(rr, Br(focused ? kAccent : kBorder), focused ? Px(1.5f) : Px(1));

    std::wstring shown = *text;
    if (password) shown.assign(shown.size(), L'\x2022');
    dc_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    if (shown.empty() && !focused) {
        Text(placeholder, fmtBody_, Inset(r, 12, 0), WithAlpha(kTextDim, 0.8f));
    } else {
        Text(shown, fmtBody_, Inset(r, 12, 0), kText);
    }
    if (focused) {
        // caret at the end of the text
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite_->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), fmtBody_.Get(),
                                                10000, r.bottom - r.top,
                                                layout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            float x = r.left + Px(12) + m.widthIncludingTrailingWhitespace;
            bool on = (GetTickCount64() / 500) % 2 == 0;
            if (on)
                dc_->FillRectangle(D2D1::RectF(x, r.top + Px(9), x + Px(1.5f), r.bottom - Px(9)),
                                   Br(kAccent));
        }
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
    if (!sessionRunning_ && !stats_.failed) errorShown_ = false;

    RefreshPreview();

    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->Clear(kBg);

    hits_.clear();
    comboRects_.clear();
    sliderRects_.clear();
    fieldRects_.clear();
    hasPopup_ = false;

    DrawHeader();
    DrawPreview();
    DrawStats();
    DrawLeftControls();
    DrawRightControls();
    DrawTransport();
    DrawActivityStrip();
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
    Text(L"Ulticast", fmtTitle_, S(46, 0, 300, 52), kText);

    std::wstring label;
    D2D1_COLOR_F color;
    if (stats_.failed) {
        label = L"ERROR";
        color = kRec;
    } else if (!sessionRunning_) {
        label = L"IDLE";
        color = kTextDim;
    } else if (stats_.paused) {
        label = L"PAUSED";
        color = kWarn;
    } else if (stats_.reconnecting) {
        label = L"RECONNECTING";
        color = kWarn;
    } else if (settings_.stream) {
        label = L"LIVE";
        color = kAccent2;
    } else {
        label = L"RECORDING";
        color = kRec;
    }
    float pillW = MeasureText(label, fmtPill_.Get()) + Px(29);
    D2D1_RECT_F pillR =
        D2D1::RectF(Px(kWinW - 20) - pillW, Px(14), Px(kWinW - 20), Px(14) + Px(24));
    Pill(pillR, color, label);
    dc_->FillRectangle(S(0, 51, kWinW, 1), Br(kBorder));}

void UiApp::DrawPreview() {
    D2D1_RECT_F r = S(20, 64, 560, 350);
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
        Text(L"NO SIGNAL", fmtSection_, c1, kTextDim);
        Text(L"Press Start to see the live scene", fmtSmall_, c2, WithAlpha(kTextDim, 0.7f));
    }

    if (sessionRunning_ && stats_.width) {
        double fps = stats_.elapsed > 0.1 ? stats_.captured / stats_.elapsed : 0.0;
        std::wstring info = Format(L"%u x %u   ·   %.0f fps", stats_.width, stats_.height, fps);
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
        float pulse = 0.55f + 0.45f * (float)sin(GetTickCount64() / 320.0);
        D2D1_COLOR_F dot = WithAlpha(settings_.stream ? kAccent2 : kRec, pulse);
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + Px(20), r.top + Px(20)), Px(6), Px(6)),
                         Br(dot));
    }
}

void UiApp::DrawStats() {
    D2D1_RECT_F r = S(20, 428, 560, 76);
    Panel(r, 12, WithAlpha(kPanel, 0.65f), false);
    Text(FormatTime(stats_.elapsed), fmtMono_, S(38, 434, 200, 36), kText);

    double fps = stats_.elapsed > 0.1 ? stats_.captured / stats_.elapsed : 0.0;
    std::wstring line1 = Format(L"%llu frames  ·  %.1f fps  ·  %.1f Mbps",
                                (unsigned long long)stats_.encoded, fps, stats_.kbps / 1000.0);
    Text(line1, fmtSmall_, S(38, 470, 520, 18), kTextDim);

    std::wstring audio = L"no audio";
    if (!stats_.desktopDevice.empty() && stats_.desktopDevice != L"-")
        audio = stats_.desktopDevice;
    if (!stats_.micDevice.empty() && stats_.micDevice != L"-") audio += L" + " + stats_.micDevice;
    std::wstring line2 = Widen(stats_.backend) + L"  ·  ";
    if (stats_.width) line2 += Format(L"%ux%u  ·  ", stats_.width, stats_.height);
    line2 += audio;
    if (!stats_.webcamName.empty()) line2 += L"  ·  cam " + stats_.webcamName;
    dc_->PushAxisAlignedClip(S(38, 488, 524, 16), D2D1_ANTIALIAS_MODE_ALIASED);
    Text(line2, fmtSmall_, S(38, 486, 524, 16), WithAlpha(kTextDim, 0.75f));
    dc_->PopAxisAlignedClip();
}

void UiApp::DrawLeftControls() {
    const float x = 20;
    float y = 522;
    SectionLabel(L"OUTPUT", x, y);
    y += 22;
    Toggle(ID_TOGGLE_RECORD, S(x, y, 100, 40), L"Record", &settings_.record);
    D2D1_RECT_F pathR = S(x + 108, y, 300, 40);
    Panel(pathR, 8, kPanel, true);
    dc_->PushAxisAlignedClip(pathR, D2D1_ANTIALIAS_MODE_ALIASED);
    Text(ShortPath(settings_.recordPath), fmtSmall_, Inset(pathR, 12, 0),
         settings_.record ? kText : kTextDim);
    dc_->PopAxisAlignedClip();
    Button(ID_BROWSE, S(x + 416, y, 110, 40), L"Browse", BtnStyle::Ghost, settings_.record);
    y += 40 + 18;

    SectionLabel(L"CLIPS", x, y);
    y += 22;
    Toggle(ID_TOGGLE_REPLAY, S(x, y, 96, 40), L"Replay", &settings_.replay);
    Slider(ID_SLIDER_REPLAY, S(x + 104, y, 250, 40), 10, 120, &settings_.replaySeconds, L"%ds");
    Text(L"F9 saves the last moments", fmtSmall_, S(x + 364, y, 196, 40),
         WithAlpha(kTextDim, 0.8f));
}

void UiApp::DrawRightControls() {
    const float x = 600, w = 400;
    const float rowH = 40, gap = 8, sectionGap = 18;
    float y = 62;

    SectionLabel(L"SOURCE", x, y);
    y += 22;
    Combo(ID_COMBO_MONITOR, S(x, y, 252, rowH), monitors_, &selMonitor_, selWindow_ == 0);
    Combo(ID_COMBO_FPS, S(x + 260, y, 140, rowH), fpss_, &selFps_);
    y += rowH + gap;
    Combo(ID_COMBO_WINDOW, S(x, y, 252, rowH), windows_, &selWindow_);
    Combo(ID_COMBO_BACKEND, S(x + 260, y, 140, rowH), backends_, &selBackend_);
    y += rowH + sectionGap;

    SectionLabel(L"CAMERA & AUDIO", x, y);
    y += 22;
    Toggle(ID_TOGGLE_WEBCAM, S(x, y, 112, rowH), L"Webcam", &settings_.webcam);
    Combo(ID_COMBO_WEBCAMDEV, S(x + 120, y, 280, rowH), webcams_, &selWebcam_, settings_.webcam);
    y += rowH + gap;
    Toggle(ID_TOGGLE_DESKTOP, S(x, y, 116, rowH), L"Desktop", &settings_.desktopAudio);
    Toggle(ID_TOGGLE_MIC, S(x + 124, y, 80, rowH), L"Mic", &settings_.mic);
    Combo(ID_COMBO_MICDEV, S(x + 212, y, 188, rowH), mics_, &selMic_, settings_.mic);
    y += rowH + gap;
    Toggle(ID_TOGGLE_CHROMA, S(x, y, 108, rowH), L"Key", &settings_.chroma);
    Field(ID_FIELD_CHROMA, S(x + 116, y, 144, rowH), &chromaText_, false, L"00FF00");
    Combo(ID_COMBO_POS, S(x + 268, y, 132, rowH), positions_, &selPos_, settings_.webcam);
    y += rowH + sectionGap;

    SectionLabel(L"VIDEO", x, y);
    y += 22;
    Combo(ID_COMBO_CODEC, S(x, y, 190, rowH), codecs_, &selCodec_);
    Combo(ID_COMBO_PRESET, S(x + 198, y, 202, rowH), presets_, &selPreset_);
    y += rowH + gap;
    Text(L"CQ", fmtBody_, S(x, y, 30, rowH), kTextDim);
    Slider(ID_SLIDER_CQP, S(x + 36, y, 364, rowH), 16, 30, (int*)&settings_.cqp, L"CQ %d");
    y += rowH + sectionGap;

    SectionLabel(L"STREAM", x, y);
    y += 22;
    Combo(ID_COMBO_PLATFORM, S(x, y, 190, rowH), platforms_, &selPlatform_);
    Toggle(ID_TOGGLE_STREAM, S(x + 198, y, 202, rowH), L"Go Live", &settings_.stream);
    y += rowH + gap;
    if (platformKeys_[selPlatform_] == "custom") {
        Field(ID_FIELD_URL, S(x, y, 400, rowH), &urlText_, false, L"rtmp:// ingest URL");
    } else {
        Field(ID_FIELD_KEY, S(x, y, 400, rowH), &keyText_, true, L"Stream key (paste)");
    }
    y += rowH + sectionGap;

    SectionLabel(L"OVERLAY", x, y);
    y += 22;
    Toggle(ID_TOGGLE_BROWSER, S(x, y, 108, rowH), L"Browser", &settings_.browser);
    Field(ID_FIELD_BROWSER, S(x + 116, y, 284, rowH), &browserText_, false,
          L"HTML file or https:// URL");
}

void UiApp::DrawTransport() {
    bool running = sessionRunning_;
    Button(ID_START, S(20, 736, 140, 44), running ? L"Stop" : L"Start",
           running ? BtnStyle::Danger : BtnStyle::Primary);
    Button(ID_PAUSE, S(168, 736, 100, 44), stats_.paused ? L"Resume" : L"Pause", BtnStyle::Ghost,
           running);
    Button(ID_CLIP, S(276, 736, 112, 44), L"Snap Clip", BtnStyle::Ghost,
           running && settings_.replay);
    Button(ID_OPENLOGS, S(400, 736, 86, 44), L"Logs", BtnStyle::Ghost);
    Button(ID_REPORT, S(494, 736, 116, 44), L"Report Bug", BtnStyle::Ghost);
    Button(ID_OPENFOLDER, S(868, 736, 132, 44), L"Open Folder", BtnStyle::Ghost,
           settings_.record);
}

void UiApp::DrawActivityStrip() {
    D2D1_RECT_F r = S(20, 700, 980, 28);
    std::vector<std::wstring> lines;
    {
        std::lock_guard<std::mutex> lock(logMu_);
        for (auto it = logLines_.rbegin(); it != logLines_.rend() && lines.size() < 2; ++it)
            lines.push_back(*it);
        std::reverse(lines.begin(), lines.end());
    }
    if (!statusText_.empty()) {
        if (lines.size() < 2)
            lines.push_back(statusText_);
        else
            lines.back() = statusText_;
    }
    if (lines.empty()) {
        Text(L"Ready", fmtSmall_, r, WithAlpha(kTextDim, 0.55f));
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
    float listH = itemH * popup_.items.size() + 8;
    D2D1_RECT_F list = D2D1::RectF(popup_.rect.left, popup_.rect.bottom + Px(4),
                                   popup_.rect.right, popup_.rect.bottom + Px(4) + Px(listH));
    if (list.bottom > Px(kWinH) - Px(20))
        list = D2D1::RectF(popup_.rect.left, popup_.rect.top - Px(4) - Px(listH),
                           popup_.rect.right, popup_.rect.top - Px(4));
    Panel(list, 8, kPanel2, true);

    for (size_t i = 0; i < popup_.items.size(); ++i) {
        D2D1_RECT_F item = D2D1::RectF(list.left + Px(4), list.top + Px(4) + Px(itemH * i),
                                       list.right - Px(4), list.top + Px(4) + Px(itemH * (i + 1)));
        hits_.push_back({ID_ITEM_BASE + (int)i, item, true});
        bool hovered = hover_ == ID_ITEM_BASE + (int)i;
        bool selected = popup_.sel && (int)i == *popup_.sel;
        if (hovered || selected)
            dc_->FillRoundedRectangle(D2D1::RoundedRect(item, Px(6), Px(6)),
                                      Br(WithAlpha(kAccent, selected ? 0.35f : 0.2f)));
        dc_->PushAxisAlignedClip(item, D2D1_ANTIALIAS_MODE_ALIASED);
        Text(popup_.items[i], fmtBody_, Inset(item, 10, 0), kText);
        dc_->PopAxisAlignedClip();
    }
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

    mics_.clear();
    mics_.push_back(L"Default microphone");
    for (auto& d : WasapiCapture::ListCaptureDevices()) mics_.push_back(d.name);

    backends_ = {L"Auto (smart)", L"WGC (cursor)", L"DXGI (fullscreen games)"};
    backendKeys_ = {"auto", "wgc", "dxgi"};
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
    if (!settings_.windowTitle.empty()) selWindow_ = find(windows_, Widen(settings_.windowTitle));
}

void UiApp::SyncSettingsFromUi() {
    settings_.monitor = selMonitor_;
    settings_.windowTitle = (selWindow_ > 0 && selWindow_ < (int)windows_.size())
                                ? Narrow(windows_[selWindow_])
                                : "";
    settings_.backend = backendKeys_[selBackend_];
    settings_.fps = fpsValues_[selFps_];
    settings_.hevc = codecHevc_[selCodec_];
    settings_.preset = presetValues_[selPreset_];
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
    cfg.monitor = selMonitor_;
    if (selWindow_ > 0 && selWindow_ < (int)windows_.size()) cfg.window = windows_[selWindow_];
    cfg.backend = settings_.backend;
    cfg.fps = settings_.fps;
    cfg.record = settings_.record;
    cfg.recordPath = settings_.recordPath;
    cfg.stream = settings_.stream;
    cfg.streamUrl = BuildStreamUrl();
    cfg.streamBitrate = 6000;
    cfg.reconnect = 5;
    cfg.hevc = settings_.hevc;
    cfg.cqp = settings_.cqp;
    cfg.preset = settings_.preset;
    cfg.audio.desktop = settings_.desktopAudio;
    cfg.audio.mic = settings_.mic;
    cfg.audio.desktopDevice = L"default";
    cfg.audio.micDevice =
        (selMic_ > 0 && selMic_ < (int)mics_.size()) ? mics_[selMic_] : L"default";
    cfg.audio.tracks = 1;
    cfg.webcam = settings_.webcam;
    cfg.webcamDevice = (selWebcam_ > 0 && selWebcam_ < (int)webcams_.size())
                           ? webcams_[selWebcam_]
                           : L"default";
    cfg.webcamPos = settings_.webcamPos;
    cfg.replay = settings_.replay;
    cfg.replaySeconds = settings_.replaySeconds;
    cfg.replayPrefix = settings_.replayPrefix;
    cfg.chroma = settings_.chroma;
    cfg.chromaColor = settings_.chromaColor;
    cfg.chromaLayer = 0;  // UI keys the webcam overlay
    cfg.browser = settings_.browser && !browserText_.empty();
    cfg.browserUrl = Narrow(browserText_);
    return cfg;
}

void UiApp::AddLog(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(logMu_);
    logLines_.push_back(s);
    if (logLines_.size() > 50) logLines_.pop_front();
}

void UiApp::StartSession() {
    SyncSettingsFromUi();
    SaveUiSettings(settings_);

    if (!settings_.record && !settings_.stream && !settings_.replay) {
        statusText_ = L"Enable Record, Go Live or Replay first";
        statusError_ = true;
        return;
    }
    if (settings_.stream && selPlatform_ != 3 && keyText_.empty()) {
        statusText_ = L"Enter a stream key to go live";
        statusError_ = true;
        return;
    }
    if (settings_.stream && selPlatform_ == 3 && urlText_.empty()) {
        statusText_ = L"Enter the RTMP ingest URL";
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
    if (settings_.browser && browserText_.empty()) {
        statusText_ = L"Browser overlay needs an HTML file or URL";
        statusError_ = true;
        return;
    }

    SessionConfig cfg = BuildConfig();
    std::string err;
    if (!session_.Start(cfg, &err)) {
        statusText_ = Widen(err);
        statusError_ = true;
        AddLog(L"Start failed: " + statusText_);
    } else {
        statusText_.clear();
        statusError_ = false;
        AddLog(settings_.stream ? L"Recording + streaming started" : L"Recording started");
    }
}

void UiApp::StopSession() {
    session_.Stop();
    stats_ = session_.GetStats();
    sessionRunning_ = false;
    AddLog(L"Stopped");
}

void UiApp::UpdateWindowTitleState() {
    std::wstring t = L"Ulticast";
    if (sessionRunning_) t += stats_.paused ? L" — paused" : L" — recording";
    SetWindowTextW(hwnd_, t.c_str());
}

void UiApp::OnAction(int id) {
    if (id >= ID_ITEM_BASE) {
        int index = id - ID_ITEM_BASE;
        if (openCombo_ >= 0 && index >= 0 && index < (int)popup_.items.size()) {
            if (popup_.sel) *popup_.sel = index;
            if (openCombo_ == ID_COMBO_WINDOW) {
                // refresh window list on demand
            }
        }
        openCombo_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    switch (id) {
        case ID_START:
            if (sessionRunning_)
                StopSession();
            else
                StartSession();
            break;
        case ID_PAUSE:
            session_.SetPaused(!session_.Paused());
            break;
        case ID_CLIP:
            session_.SaveClip();
            break;
        case ID_BROWSE: {
            wchar_t file[MAX_PATH] = {};
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
        case ID_TOGGLE_RECORD:
        case ID_TOGGLE_STREAM:
        case ID_TOGGLE_WEBCAM:
        case ID_TOGGLE_DESKTOP:
        case ID_TOGGLE_MIC:
        case ID_TOGGLE_REPLAY:
            // value already toggled by the widget handler
            if (id == ID_TOGGLE_STREAM && settings_.stream && selPlatform_ != 3 &&
                keyText_.empty()) {
                statusText_ = L"Enter a stream key first";
                statusError_ = true;
            }
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
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show Ulticast");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (sessionRunning_ ? MF_GRAYED : 0), ID_TRAY_START, L"Start");
    AppendMenuW(menu, MF_STRING | (!sessionRunning_ ? MF_GRAYED : 0), ID_TRAY_START + 100, L"Stop");
    AppendMenuW(menu, MF_STRING | (!sessionRunning_ ? MF_GRAYED : 0), ID_TRAY_PAUSE,
                stats_.paused ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_STRING | (!sessionRunning_ || !settings_.replay ? MF_GRAYED : 0),
                ID_TRAY_CLIP, L"Snap Clip");
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
        case ID_TRAY_START:
            StartSession();
            break;
        case ID_TRAY_START + 100:
            StopSession();
            break;
        case ID_TRAY_PAUSE:
            session_.SetPaused(!session_.Paused());
            break;
        case ID_TRAY_CLIP:
            session_.SaveClip();
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
                        if (h.id == ID_FIELD_KEY || h.id == ID_FIELD_URL)
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
            if (press_ == ID_SLIDER_CQP || press_ == ID_SLIDER_REPLAY) {
                dragging_ = press_;
                SetSliderFromX(press_, x);
            }
            SetCapture(hwnd_);

            // text fields: focus on click
            int newFocus = 0;
            for (int fid : {ID_FIELD_KEY, ID_FIELD_URL, ID_FIELD_CHROMA, ID_FIELD_BROWSER}) {
                auto it = fieldRects_.find(fid);
                if (it != fieldRects_.end() && x >= it->second.left && x < it->second.right &&
                    y >= it->second.top && y < it->second.bottom)
                    newFocus = fid;
            }
            if (newFocus != focus_) {
                focus_ = newFocus;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            // toggles act immediately
            for (int tid : {ID_TOGGLE_RECORD, ID_TOGGLE_STREAM, ID_TOGGLE_WEBCAM, ID_TOGGLE_DESKTOP,
                            ID_TOGGLE_MIC, ID_TOGGLE_REPLAY, ID_TOGGLE_CHROMA,
                            ID_TOGGLE_BROWSER}) {
                if (press_ == tid) {
                    bool* target = nullptr;
                    switch (tid) {
                        case ID_TOGGLE_RECORD: target = &settings_.record; break;
                        case ID_TOGGLE_STREAM: target = &settings_.stream; break;
                        case ID_TOGGLE_WEBCAM: target = &settings_.webcam; break;
                        case ID_TOGGLE_DESKTOP: target = &settings_.desktopAudio; break;
                        case ID_TOGGLE_MIC: target = &settings_.mic; break;
                        case ID_TOGGLE_REPLAY: target = &settings_.replay; break;
                        case ID_TOGGLE_CHROMA: target = &settings_.chroma; break;
                        case ID_TOGGLE_BROWSER: target = &settings_.browser; break;
                    }
                    if (target) *target = !*target;
                    OnAction(tid);
                    press_ = 0;
                }
            }

            // combos open/close
            for (auto& [id, rect] : comboRects_) {
                if (press_ == id) {
                    openCombo_ = (openCombo_ == id) ? -1 : id;
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
            ReleaseCapture();
            float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
            dragging_ = 0;
            int released = 0;
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
                if (it->enabled && x >= it->rect.left && x < it->rect.right && y >= it->rect.top &&
                    y < it->rect.bottom) {
                    released = it->id;
                    break;
                }
            }
            if (released && released == press_) OnAction(released);
            else if (openCombo_ >= 0 && released != openCombo_ &&
                     !(released >= ID_ITEM_BASE && released < ID_ITEM_BASE + 64)) {
                openCombo_ = -1;
            }
            press_ = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            press_ = 0;
            dragging_ = 0;
            return 0;
        case WM_CHAR: {
            wchar_t c = (wchar_t)wp;
            std::wstring* target = nullptr;
            if (focus_ == ID_FIELD_KEY) target = &keyText_;
            else if (focus_ == ID_FIELD_URL) target = &urlText_;
            else if (focus_ == ID_FIELD_CHROMA) target = &chromaText_;
            else if (focus_ == ID_FIELD_BROWSER) target = &browserText_;
            if (!target) return 0;
            if (c == 8) {  // backspace
                if (!target->empty()) target->pop_back();
            } else if (c == 22) {  // Ctrl+V
                if (OpenClipboard(hwnd)) {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    if (h) {
                        auto* text = (wchar_t*)GlobalLock(h);
                        if (text) {
                            *target += text;
                            GlobalUnlock(h);
                        }
                    }
                    CloseClipboard();
                }
                // Strip control characters: pasted keys/URLs often carry CR/LF.
                target->erase(std::remove_if(target->begin(), target->end(),
                                             [](wchar_t ch) { return ch < 32 || ch == 127; }),
                              target->end());
            } else if (c >= 32) {
                *target += c;
            }
            if (focus_ == ID_FIELD_CHROMA) {
                // keep it a clean 6-digit hex color
                std::wstring filtered;
                for (wchar_t ch : chromaText_)
                    if (iswxdigit(ch) && filtered.size() < 6)
                        filtered += (wchar_t)towupper(ch);
                chromaText_ = filtered;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (focus_ && wp == VK_ESCAPE) {
                focus_ = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
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
            if (exiting_) DestroyWindow(hwnd);
            else ShowWindow(hwnd, SW_HIDE);  // keep running in the tray
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
