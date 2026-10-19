#include "browser_cef.hpp"
#include "diag.hpp"
#include "log.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
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
// Browser lifetime: the close is asynchronous, so shutdown waits for
// OnBeforeClose before calling CefShutdown (otherwise Chromium waits forever
// and the process never exits). Identity is tracked by CefBrowser's integer
// identifier: CEF hands out different interface pointers for the same browser
// (OnAfterCreated vs OnBeforeClose), so raw pointer comparison is unreliable.
bool g_browserAlive = false;
int g_browserId = 0;
std::condition_variable g_browserGone;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
}

std::string NormalizeUrl(const std::string& in) {
    if (in.rfind("http://", 0) == 0 || in.rfind("https://", 0) == 0 ||
        in.rfind("file://", 0) == 0 || in.rfind("data:", 0) == 0 ||
        in.rfind("about:", 0) == 0)
        return in;
    // A bare host ("example.com", "localhost:8080/x") is a web address, not a
    // file: typing a domain should not end up as file://example.com.
    const bool looksLikeHost = in.find(' ') == std::string::npos &&
                               in.find(':') != 1 &&  // not a drive letter
                               in.find('.') != std::string::npos;
    if (looksLikeHost) return "https://" + in;
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

// Forward declaration: the client reports load status through this (defined
// below, next to the sink that forwards it to the app log / activity strip).
static void ReportStatus(const std::string& message);

class OverlayClient : public CefClient,
                      public CefRenderHandler,
                      public CefLifeSpanHandler,
                      public CefLoadHandler {
public:
    std::function<void(const void*, int, int)> onFrame;
    int viewW = 1280, viewH = 720;

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

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
        g_browserId = browser->GetIdentifier();
        g_browserAlive = true;
        Diag::Log("[browser] browser created (id %d)", g_browserId);
    }
    bool DoClose(CefRefPtr<CefBrowser>) override { return false; }  // windowless: destroy now
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            // Only clear our reference if it is still the browser being closed:
            // a Stop/Start cycle can have created a newer one.
            if (browser->GetIdentifier() == g_browserId) {
                g_browser = nullptr;
                g_browserId = 0;
                g_browserAlive = false;
                Diag::Log("[browser] browser closed");
            }
        }
        g_browserGone.notify_all();
    }

    // CefLoadHandler: report the outcome so a blank overlay is never silent.
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int httpStatusCode) override {
        if (frame && frame->IsMain()) ReportStatus("loaded (HTTP " + std::to_string(httpStatusCode) + ")");
    }
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                     const CefString& errorText, const CefString& failedUrl) override {
        if (!frame || !frame->IsMain()) return;
        ReportStatus("load failed: " + errorText.ToString() + " (" + failedUrl.ToString() + ")");
    }

    IMPLEMENT_REFCOUNTING(OverlayClient);
};

// Load-status sink: the session forwards it to the log/activity strip.
static std::mutex g_statusMu;
static std::function<void(const std::string&)> g_statusFn;

static void ReportStatus(const std::string& message) {
    Diag::Log("[browser] %s", message.c_str());
    std::function<void(const std::string&)> fn;
    {
        std::lock_guard<std::mutex> lock(g_statusMu);
        fn = g_statusFn;
    }
    if (fn) fn(message);
}

}  // namespace

void BrowserOverlay::SetStatusFn(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lock(g_statusMu);
    g_statusFn = std::move(fn);
}

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
        g_client = nullptr;
    }
    if (browser) browser->GetHost()->CloseBrowser(true);
    // Wait for OnBeforeClose: with a multi-threaded message loop CefShutdown
    // blocks until every browser is gone, so calling it right after
    // CloseBrowser(true) leaves the process hanging with no window on screen.
    {
        std::unique_lock<std::mutex> lock(g_mu);
        if (g_browserAlive &&
            !g_browserGone.wait_for(lock, std::chrono::seconds(5),
                                    [] { return !g_browserAlive; }))
            Diag::Log("[browser] overlay close timed out; shutting down anyway");
        g_browser = nullptr;
        g_browserId = 0;
        g_browserAlive = false;
    }
    // CefShutdown must not run while holding g_mu: CEF's UI thread callbacks
    // (OnAfterCreated/OnBeforeClose) also take it.
    if (g_initialized.exchange(false)) {
        Diag::Log("[browser] stopping the runtime");
        CefShutdown();
        Diag::Log("[browser] runtime stopped");
    }
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
        // Keep Chromium's own log next to ours: overlay problems (network, TLS,
        // page errors) then show up in diagnostics instead of failing silently.
        settings.log_severity = LOGSEVERITY_INFO;
        std::wstring cefLog = Widen(Diag::LogDir() + "\\cef.log");
        std::error_code ec;
        if (std::filesystem::exists(cefLog, ec) &&
            std::filesystem::file_size(cefLog, ec) > 5u * 1024 * 1024)
            std::filesystem::remove(cefLog, ec);  // INFO logging must not grow forever
        CefString(&settings.log_file) = cefLog;
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

void BrowserOverlay::LoadUrl(const std::string& url) {
    // CEF frame methods must run on its own UI thread; calling LoadURL from the
    // app thread can block it and freeze the UI. Restarting the overlay is the
    // safe path (and it is what the preview restart already does).
    if (!started_.load()) return;
    Stop();
    std::string err;
    Start(url, (int)width_, (int)height_, &err);
    ReportStatus("loading " + url);
}

void BrowserOverlay::Reload() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_browser) {
        g_browser->Reload();
        ReportStatus("reloading");
    }
}

void BrowserOverlay::Stop() {
    if (!started_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(sink_->mu);
        sink_->fn = nullptr;  // the browser closes asynchronously: stop deliveries now
    }
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        browser = g_browser;
        g_client = nullptr;
    }
    if (browser) {
        browser->GetHost()->CloseBrowser(true);
        // The close is asynchronous (OnBeforeClose runs on CEF's UI thread).
        // Wait so a Stop/Start cycle cannot race the old browser's teardown.
        std::unique_lock<std::mutex> lock(g_mu);
        if (!g_browserGone.wait_for(lock, std::chrono::seconds(3),
                                    [] { return !g_browserAlive; })) {
            Diag::Log("[browser] overlay close timed out in Stop()");
            g_browser = nullptr;
            g_browserId = 0;
            g_browserAlive = false;
        }
    }
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
