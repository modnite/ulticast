#include "ui_app.hpp"
#include "../core/browser_cef.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Per-monitor DPI awareness before any window/CEF work so layout scaling
    // and WM_DPICHANGED actually engage.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int cefRc = BrowserOverlay::ExecuteSubprocess(instance);
    if (cefRc >= 0) return cefRc;

    // Single instance: a second copy would fight over the camera, the settings
    // file and the tray icon, and would confuse the installer's process cleanup.
    // (CEF helper processes return above and never reach this.)
    HANDLE once = CreateMutexW(nullptr, TRUE, L"UlticastUi.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"UlticastUiWindow", nullptr)) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    // Intentionally not closed: the handle lives for the process lifetime and
    // the OS releases the mutex when the process exits.

    return RunUiApp(instance);
}
