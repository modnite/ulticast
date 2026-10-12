#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "capture_common.hpp"

// DXGI Desktop Duplication backend.
//
// Unlike WGC, duplication only delivers frames when the desktop changes, so
// this backend runs its own paced loop: on every tick it acquires a frame if
// one is ready, otherwise it re-submits the retained previous frame. That gives
// constant frame rate output even on a static screen -- and unlike WGC, this
// path can capture true fullscreen-exclusive games.
//
// The hardware cursor is reported separately by duplication; the state below is
// read by the capture callback (same thread) and composited by the Session.
class DxgiDuplicator {
public:
    struct CursorState {
        bool visible = false;
        int x = 0, y = 0;
        bool hasShape = false;
        uint32_t shapeType = 0;  // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_*
        uint32_t width = 0, height = 0, pitch = 0;
        int hotspotX = 0, hotspotY = 0;
        std::vector<uint8_t> shape;
        uint64_t shapeVersion = 0;
    };

    bool Start(int monitorIndex, ID3D11Device* device, uint32_t fps, FrameCallback cb,
               bool requireContent, std::string* err);
    void Stop();

    std::atomic<uint64_t> frameCount{0};   // ticks (paced frames submitted)
    std::atomic<uint64_t> newFrames{0};    // real updates received from duplication
    std::atomic<uint64_t> pointerUpdates{0};
    std::atomic<uint64_t> shapeUpdates{0};
    uint32_t width = 0;
    uint32_t height = 0;
    int originX = 0, originY = 0;  // monitor origin in virtual-screen coordinates
    std::wstring monitorName;

    // Valid only from the capture callback (same thread that updates it).
    const CursorState& Cursor() const { return cursor_; }

private:
    void ThreadMain();
    bool Run(std::string* err);
    bool AcquireOutput();
    bool EnsureRetained();
    bool SeedFrame();
    bool ProbeContent();

    std::thread thread_;
    std::atomic<bool> quit_{false};
    HANDLE hReady_ = nullptr;
    std::string initError_;
    int monitorIndex_ = 0;
    uint32_t fps_ = 60;
    bool requireContent_ = true;
    bool contentOk_ = true;
    ID3D11Device* device_ = nullptr;
    FrameCallback cb_;
    CursorState cursor_;

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> retained_;
    int outputIndex_ = -1;
};
