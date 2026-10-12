#include "diag.hpp"
#include "version.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "capture_wgc.hpp"
#include "d3d.hpp"
#include "log.hpp"
#include "wasapi_capture.hpp"
#include "webcam_mf.hpp"

namespace {

std::mutex g_mu;
std::ofstream g_file;
std::string g_logPath;

std::string Timestamp(bool forFile) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    sprintf_s(buf, forFile ? "%04d%02d%02d-%02d%02d%02d" : "%04d-%02d-%02d %02d:%02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string RegString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return Narrow(buf);
}

std::string FileVersion(const std::wstring& path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", (void**)&info, &len) || !info) return {};
    char buf[64];
    sprintf_s(buf, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
              HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buf;
}

std::string ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    return Narrow(slash == std::wstring::npos ? dir : dir.substr(0, slash + 1));
}

std::string RedactedSettings() {
    const char* appdata = getenv("APPDATA");
    if (!appdata) return "(no %APPDATA%)";
    std::ifstream f(std::string(appdata) + "\\Ulticast\\settings.ini");
    if (!f) return "(no settings file yet)";
    std::string out, line;
    while (std::getline(f, line)) {
        if (line.rfind("streamKeyEnc=", 0) == 0) line = "streamKeyEnc=<redacted>";
        out += "  " + line + "\n";
    }
    return out;
}

std::string LogTail(size_t maxLines) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_logPath.empty()) return "(logging not started)";
    std::ifstream f(g_logPath);
    if (!f) return "(cannot read log)";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
        if (lines.size() > maxLines) lines.erase(lines.begin());
    }
    std::string out;
    for (auto& l : lines) out += "  " + l + "\n";
    return out;
}

}  // namespace

namespace Diag {

std::string DataDir() {
    const char* local = getenv("LOCALAPPDATA");
    return local ? std::string(local) + "\\Ulticast" : ".";
}

std::string LogDir() {
    std::string dir = DataDir() + "\\logs";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string LogPath() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_logPath;
}

void StartLog(const std::string& appTag) {
    std::lock_guard<std::mutex> lock(g_mu);

    // Keep the newest 20 logs.
    std::vector<std::filesystem::directory_entry> logs;
    for (auto& e : std::filesystem::directory_iterator(LogDir()))
        if (e.path().extension() == ".log") logs.push_back(e);
    std::sort(logs.begin(), logs.end(), [](auto& a, auto& b) {
        return a.last_write_time() > b.last_write_time();
    });
    for (size_t i = 20; i < logs.size(); ++i) {
        std::error_code ec;
        std::filesystem::remove(logs[i].path(), ec);
    }

    g_logPath = LogDir() + "\\" + appTag + "-" + Timestamp(true) + ".log";
    g_file.open(g_logPath, std::ios::app);
    if (g_file) {
        g_file << "Ulticast " << ULTI_VERSION << " (" << appTag << ") started "
               << Timestamp(false) << "\n";
        g_file.flush();
    }
}

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    std::printf("%s\n", buf);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) {
        g_file << Timestamp(false) << "  " << buf << "\n";
        g_file.flush();
    }
}

std::string BuildReport() {
    std::string s;
    s += "Ulticast diagnostics\n";
    s += "====================\n";
    s += "Version    : " ULTI_VERSION "\n";
    s += "Built      : " __DATE__ " " __TIME__ "\n";
    s += "Report time: " + Timestamp(false) + "\n\n";

    std::string product = RegString(HKEY_LOCAL_MACHINE,
                                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                    L"ProductName");
    std::string release = RegString(HKEY_LOCAL_MACHINE,
                                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                    L"DisplayVersion");
    std::string build = RegString(HKEY_LOCAL_MACHINE,
                                  L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                  L"CurrentBuild");
    s += "OS         : " + product + " " + release + " (build " + build + ")\n";
    s += "CPU        : " + RegString(HKEY_LOCAL_MACHINE,
                                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                     L"ProcessorNameString") +
         "\n";
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        char buf[96];
        sprintf_s(buf, "%.1f GB total, %.1f GB available", mem.ullTotalPhys / 1073741824.0,
                  mem.ullAvailPhys / 1073741824.0);
        s += std::string("RAM        : ") + buf + "\n";
    }

    for (auto& a : D3DContext::ListAdapters())
        s += "GPU        : " + Narrow(a.name) + "\n";
    wchar_t sysdir[MAX_PATH] = {};
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::string nvenc = FileVersion(std::wstring(sysdir) + L"\\nvEncodeAPI64.dll");
    if (nvenc.empty()) nvenc = FileVersion(Widen(ExeDir()) + L"nvEncodeAPI64.dll");
    s += "NVENC API  : " + (nvenc.empty() ? "(driver DLL not found)" : nvenc) + "\n";

    s += "\nMonitors:\n";
    for (auto& m : WgcCapture::ListMonitors()) {
        char buf[160];
        sprintf_s(buf, "  [%d] %ldx%ld at (%ld,%ld) %s", m.index, m.rect.right - m.rect.left,
                  m.rect.bottom - m.rect.top, m.rect.left, m.rect.top,
                  m.primary ? "(primary)" : "");
        s += std::string(buf) + "\n";
    }
    s += "Audio outputs:\n";
    for (auto& d : WasapiCapture::ListRenderDevices())
        s += "  " + Narrow(d.name) + (d.isDefault ? " [default]" : "") + "\n";
    s += "Microphones:\n";
    for (auto& d : WasapiCapture::ListCaptureDevices())
        s += "  " + Narrow(d.name) + (d.isDefault ? " [default]" : "") + "\n";
    s += "Cameras:\n";
    for (auto& d : WebcamCapture::ListDevices()) s += "  " + Narrow(d) + "\n";

    s += "\nSettings (stream key redacted):\n";
    s += RedactedSettings();

    s += "\nLog file: " + LogPath() + "\n";
    s += "Log tail:\n" + LogTail(40);
    return s;
}

}  // namespace Diag
