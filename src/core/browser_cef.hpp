#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

struct FrameSink;

// Off-screen browser overlay (CEF). Renders a page into a BGRA buffer that the
// compositor draws as a transparent scene layer -- the same architecture OBS
// uses for browser sources (alerts, chat widgets, HTML overlays).
//
// CEF is process-global: it is initialized lazily on first use and kept alive
// until ShutdownGlobal() (called at process exit).
class BrowserOverlay {
public:
    BrowserOverlay();
    ~BrowserOverlay();

    // Call at the very top of the process entry point. Returns >= 0 when this
    // process is a CEF subprocess that has finished executing (then exit).
    static int ExecuteSubprocess(void* instance);
    static bool RuntimePresent();  // libcef.dll next to the executable?
    static void ShutdownGlobal();
    // Load status ("loaded (HTTP 200)" / "load failed: ...") -> log + UI strip.
    static void SetStatusFn(std::function<void(const std::string&)> fn);

    bool Start(const std::string& url, int width, int height, std::string* err);
    void Stop();
    // Force a reload (useful when a page failed to load or needs refreshing).
    void Reload();
    // Load a different URL in the running browser (no restart): used by the
    // Settings overlay field so the preview follows what is being typed.
    void LoadUrl(const std::string& url);

    // Latest painted frame (packed BGRA, top-left origin). frameNo increments
    // when a new frame is available.
    bool GetLatest(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height,
                   uint64_t* frameNo);

private:
    void OnPaintFrame(const void* buffer, int width, int height);

    std::shared_ptr<FrameSink> sink_;
    std::mutex mu_;
    std::vector<uint8_t> bufA_, bufB_;
    bool useA_ = true;
    uint32_t width_ = 0, height_ = 0;
    uint64_t frameNo_ = 0;
    std::atomic<bool> started_{false};
};
