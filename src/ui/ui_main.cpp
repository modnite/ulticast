#include "ui_app.hpp"
#include "../core/browser_cef.hpp"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Per-monitor DPI awareness before any window/CEF work so layout scaling
    // and WM_DPICHANGED actually engage.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int cefRc = BrowserOverlay::ExecuteSubprocess(instance);
    if (cefRc >= 0) return cefRc;
    return RunUiApp(instance);
}
