#include "browser_cef.hpp"
#include "log.hpp"

#include <cstring>
#include <filesystem>
#include <functional>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_render_handler.h"

// Shared delivery state: the browser closes asynchronously, so the paint
// callback must never dereference a dead owner.
struct FrameSink {
    std::mutex mu;
    std::function<void(const void*, int, int)> fn;
};

namespace {
std::atomic<bool> g_initialized{false};
std::mutex g_mu;
CefRefPtr<CefApp> g_app;
CefRefPtr<CefBrowser> g_browser;
CefRefPtr<class OverlayClient> g_client;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
}

std::string NormalizeUrl(const std::string& in) {
    if (in.rfind("http://", 0) == 0 || in.rfind("https://", 0) == 0 ||
        in.rfind("file://", 0) == 0 || in.rfind("data:", 0) == 0)
        return in;
    std::string path = in;
    for (auto& c : path)
        if (c == '\\') c = '/';
    return "file:///" + path;
}

class OverlayApp : public CefApp {
public:
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override {
        if (process_type.empty()) {
            // Off-screen rendering in a VM/GPU-constrained environment: keep the
            // compositor on the CPU and skip occlusion tracking.
            command_line->AppendSwitch("disable-gpu");
            command_line->AppendSwitch("disable-gpu-compositing");
            command_line->AppendSwitch("disable-features=CalculateNativeWinOcclusion");
        }
    }
    IMPLEMENT_REFCOUNTING(OverlayApp);
};

class OverlayClient : public CefClient,
                      public CefRenderHandler,
                      public CefLifeSpanHandler {
public:
    std::function<void(const void*, int, int)> onFrame;
    int viewW = 1280, viewH = 720;

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, viewW, viewH);
    }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&, const void* buffer,
                 int width, int height) override {
        if (type != PET_VIEW || !buffer || width <= 0 || height <= 0) return;
        if (onFrame) onFrame(buffer, width, height);
    }

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        std::lock_guard<std::mutex> lock(g_mu);
        g_browser = browser;
    }
    bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        std::lock_guard<std::mutex> lock(g_mu);
        // Only clear our reference if it is still the browser being closed:
        // a Stop/Start cycle can have created a newer one.
        if (g_browser && g_browser.get() == browser.get()) g_browser = nullptr;
    }

    IMPLEMENT_REFCOUNTING(OverlayClient);
};

}  // namespace

bool BrowserOverlay::RuntimePresent() {
    return GetFileAttributesW((ExeDir() + L"libcef.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

BrowserOverlay::BrowserOverlay() : sink_(std::make_shared<FrameSink>()) {}
BrowserOverlay::~BrowserOverlay() {
    Stop();
}

int BrowserOverlay::ExecuteSubprocess(void* instance) {
    if (!RuntimePresent()) return -1;
    CefMainArgs args(reinterpret_cast<HINSTANCE>(instance));
    return CefExecuteProcess(args, nullptr, nullptr);
}

void BrowserOverlay::ShutdownGlobal() {
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<OverlayClient> client;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        browser = g_browser;
        client = g_client;
        g_browser = nullptr;
        g_client = nullptr;
    }
    if (browser) browser->GetHost()->CloseBrowser(true);
    // CefShutdown must not run while holding g_mu: CEF's UI thread callbacks
    // (OnAfterCreated/OnBeforeClose) also take it.
    if (g_initialized.exchange(false)) CefShutdown();
}

bool BrowserOverlay::Start(const std::string& url, int width, int height, std::string* err) {
    if (!RuntimePresent()) {
        *err = "libcef.dll not found next to the executable";
        return false;
    }
    if (started_.load()) return true;
    width_ = (uint32_t)(width > 0 ? width : 1280);
    height_ = (uint32_t)(height > 0 ? height : 720);

    std::lock_guard<std::mutex> lock(g_mu);

    if (!g_initialized.load()) {
        CefMainArgs args(GetModuleHandleW(nullptr));
        CefSettings settings;
        settings.no_sandbox = true;
        settings.multi_threaded_message_loop = true;
        settings.windowless_rendering_enabled = true;
        settings.log_severity = LOGSEVERITY_DISABLE;
        std::wstring cache = ExeDir() + L"cef-cache";
        CefString(&settings.root_cache_path) = cache;
        g_app = new OverlayApp();
        if (!CefInitialize(args, settings, g_app.get(), nullptr)) {
            g_app = nullptr;
            *err = "CefInitialize failed";
            return false;
        }
        g_initialized = true;
    }

    auto* client = new OverlayClient();
    g_client = client;
    client->viewW = (int)width_;
    client->viewH = (int)height_;
    // Deliver frames through a shared sink whose callback is cleared in Stop():
    // the browser closes asynchronously, so it must never reach a dead owner.
    auto sink = sink_;
    client->onFrame = [sink](const void* b, int w, int h) {
        std::lock_guard<std::mutex> lock(sink->mu);
        if (sink->fn) sink->fn(b, w, h);
    };
    {
        std::lock_guard<std::mutex> lock(sink_->mu);
        sink_->fn = [this](const void* b, int w, int h) { OnPaintFrame(b, w, h); };
    }

    CefWindowInfo info;
    info.SetAsWindowless(nullptr);

    CefBrowserSettings bsettings;
    bsettings.windowless_frame_rate = 30;
    bsettings.background_color = 0;  // transparent painting for windowless browsers

    CefBrowserHost::CreateBrowser(info, client, NormalizeUrl(url), bsettings, nullptr, nullptr);

    started_ = true;
    return true;
}

void BrowserOverlay::Stop() {
    if (!started_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(sink_->mu);
        sink_->fn = nullptr;  // the browser closes asynchronously: stop deliveries now
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_browser) {
        g_browser->GetHost()->CloseBrowser(true);
        g_browser = nullptr;
    }
    g_client = nullptr;
}

void BrowserOverlay::OnPaintFrame(const void* buffer, int width, int height) {
    std::lock_guard<std::mutex> lock(mu_);
    if (width_ != (uint32_t)width || height_ != (uint32_t)height) {
        width_ = (uint32_t)width;
        height_ = (uint32_t)height;
    }
    std::vector<uint8_t>& dst = useA_ ? bufA_ : bufB_;
    dst.assign((const uint8_t*)buffer, (const uint8_t*)buffer + (size_t)width * height * 4);
    useA_ = !useA_;
    frameNo_++;
}

bool BrowserOverlay::GetLatest(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height,
                               uint64_t* frameNo) {
    std::lock_guard<std::mutex> lock(mu_);
    if (frameNo_ == *frameNo || width_ == 0) return false;
    *out = useA_ ? bufB_ : bufA_;  // the buffer most recently written
    *width = width_;
    *height = height_;
    *frameNo = frameNo_;
    return true;
}
