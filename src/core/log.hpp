#pragma once
#include <windows.h>
#include <cstdio>
#include <string>

// Small helpers: hex-formatting HRESULTs and UTF-8 <-> UTF-16 conversion
// (FFmpeg works in UTF-8, Win32 works in UTF-16).

inline std::string Hex(long hr) {
    char b[16];
    sprintf_s(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return b;
}

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
