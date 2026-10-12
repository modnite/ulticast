#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>

#include "capture_common.hpp"

struct MonitorInfo {
    int index = 0;
    HMONITOR handle = nullptr;
    RECT rect{};
    std::wstring device;
    bool primary = false;
};

// Windows.Graphics.Capture (WGC) source: monitor or window.
// WGC is the modern, low-overhead capture API -- same family of primitive the
// GPU compositor in Windows uses. No GDI, no hooks, no injection.
class WgcCapture {
public:
    static std::vector<MonitorInfo> ListMonitors();
    static std::vector<std::wstring> ListWindows();

    bool StartMonitor(int index, ID3D11Device* device, FrameCallback cb, std::string* err);
    bool StartWindow(const std::wstring& titleSubstr, ID3D11Device* device, FrameCallback cb,
                     std::string* err);
    void Stop();

    std::atomic<uint64_t> frameCount{0};

private:
    bool StartItem(winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item,
                   ID3D11Device* device, FrameCallback cb, std::string* err);

    struct Impl;
    std::shared_ptr<Impl> impl_;
};
