#include "twitch_auth.hpp"

#include <ctime>
#include <vector>

#include <windows.h>
#include <winhttp.h>

#include "log.hpp"

#pragma comment(lib, "winhttp.lib")

namespace Twitch {

namespace {

// ------------------------------------------------------------------ HTTP ----

struct HttpResponse {
    int status = 0;
    std::string body;
};

bool HttpRequest(const wchar_t* host, const wchar_t* path, const wchar_t* method,
                 const std::string& body, const std::string& extraHeaders, HttpResponse* out,
                 std::string* err) {
    HINTERNET session = WinHttpOpen(L"Ulticast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *err = "WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

    HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        *err = "WinHttpConnect failed";
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(conn, method, path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        *err = "WinHttpOpenRequest failed";
        return false;
    }

    std::wstring headers;
    if (!extraHeaders.empty()) headers = std::wstring(extraHeaders.begin(), extraHeaders.end());
    const bool hasBody = !body.empty();
    if (hasBody) headers += L"Content-Type: application/x-www-form-urlencoded\r\n";

    BOOL ok = WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                 headers.empty() ? 0 : (DWORD)-1L,
                                 hasBody ? (LPVOID)body.data() : WINHTTP_NO_REQUEST_DATA,
                                 (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!ok) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        *err = "WinHttpSendRequest failed (" + Hex(GetLastError()) + ")";
        return false;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        *err = "WinHttpReceiveResponse failed (" + Hex(GetLastError()) + ")";
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    out->status = (int)status;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
        chunk.resize(read);
        out->body += chunk;
        if (out->body.size() > 64 * 1024) break;  // responses are small; bail out
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return true;
}

// Minimal JSON field reader for the flat responses used here:
//   {"access_token":"...","refresh_token":"..."}
bool JsonStr(const std::string& json, const char* key, std::string* out) {
    const std::string pat = std::string("\"") + key + "\":\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    size_t end = p;
    while (end < json.size() && json[end] != '"') {
        if (json[end] == '\\' && end + 1 < json.size()) ++end;  // skip escapes
        ++end;
    }
    *out = json.substr(p, end - p);
    return true;
}

bool JsonInt(const std::string& json, const char* key, long long* out) {
    const std::string pat = std::string("\"") + key + "\":";
    size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    *out = _strtoi64(json.c_str() + p, nullptr, 10);
    return true;
}

std::string FormBody(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (auto& [k, v] : fields) {
        if (!body.empty()) body += '&';
        body += k + "=" + v;
    }
    return body;
}

int64_t NowUnix() { return (int64_t)time(nullptr); }

}  // namespace

// ------------------------------------------------------------- device flow --

bool RequestDeviceCode(const std::string& clientId, DeviceCode* out, std::string* err) {
    HttpResponse r;
    const std::string body = FormBody({{"client_id", clientId}, {"scopes", kScopes}});
    if (!HttpRequest(L"id.twitch.tv", L"/oauth2/device", L"POST", body, "", &r, err)) return false;
    if (r.status != 200) {
        *err = "device code request failed (HTTP " + std::to_string(r.status) + ")";
        return false;
    }
    long long interval = 5, expires = 1800;
    if (!JsonStr(r.body, "device_code", &out->deviceCode) ||
        !JsonStr(r.body, "user_code", &out->userCode) ||
        !JsonStr(r.body, "verification_uri", &out->verificationUri)) {
        *err = "device code response missing fields";
        return false;
    }
    JsonInt(r.body, "interval", &interval);
    JsonInt(r.body, "expires_in", &expires);
    out->interval = interval > 0 ? (int)interval : 5;
    out->expiresIn = expires > 0 ? (int)expires : 1800;
    return true;
}

Poll PollToken(const std::string& clientId, const DeviceCode& code, Tokens* out,
               std::string* err) {
    HttpResponse r;
    const std::string body = FormBody({{"client_id", clientId},
                                       {"scopes", kScopes},
                                       {"device_code", code.deviceCode},
                                       {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});
    if (!HttpRequest(L"id.twitch.tv", L"/oauth2/token", L"POST", body, "", &r, err))
        return Poll::Error;

    if (r.status == 200) {
        long long expires = 0;
        if (!JsonStr(r.body, "access_token", &out->access) ||
            !JsonStr(r.body, "refresh_token", &out->refresh)) {
            *err = "token response missing fields";
            return Poll::Error;
        }
        JsonInt(r.body, "expires_in", &expires);
        out->expiresAtUnix = NowUnix() + (expires > 0 ? expires : 3600);
        return Poll::Authorized;
    }

    std::string message;
    JsonStr(r.body, "message", &message);
    if (message == "authorization_pending") return Poll::Pending;
    if (message == "slow_down") return Poll::SlowDown;
    if (message == "expired_token") return Poll::Expired;
    if (message == "access_denied" || message == "user_denied") return Poll::Denied;
    *err = "token poll failed (HTTP " + std::to_string(r.status) + "): " + message;
    return Poll::Error;
}

bool Refresh(const std::string& clientId, const std::string& refreshToken, Tokens* out,
             std::string* err) {
    HttpResponse r;
    const std::string body = FormBody({{"client_id", clientId},
                                       {"grant_type", "refresh_token"},
                                       {"refresh_token", refreshToken}});
    if (!HttpRequest(L"id.twitch.tv", L"/oauth2/token", L"POST", body, "", &r, err)) return false;
    if (r.status != 200) {
        *err = "refresh failed (HTTP " + std::to_string(r.status) + ")";
        return false;
    }
    long long expires = 0;
    if (!JsonStr(r.body, "access_token", &out->access) ||
        !JsonStr(r.body, "refresh_token", &out->refresh)) {
        *err = "refresh response missing fields";
        return false;
    }
    JsonInt(r.body, "expires_in", &expires);
    out->expiresAtUnix = NowUnix() + (expires > 0 ? expires : 3600);
    return true;
}

// ------------------------------------------------------------------ Helix --

bool FetchUser(const std::string& clientId, Tokens* t, std::string* err) {
    HttpResponse r;
    const std::string headers =
        "Authorization: Bearer " + t->access + "\r\nClient-Id: " + clientId + "\r\n";
    if (!HttpRequest(L"api.twitch.tv", L"/helix/users", L"GET", "", headers, &r, err)) return false;
    if (r.status != 200) {
        *err = "users request failed (HTTP " + std::to_string(r.status) + ")";
        return false;
    }
    if (!JsonStr(r.body, "id", &t->userId) || !JsonStr(r.body, "login", &t->login) ||
        !JsonStr(r.body, "display_name", &t->displayName)) {
        *err = "users response missing fields";
        return false;
    }
    return true;
}

bool FetchStreamKey(const std::string& clientId, const Tokens& t, std::string* key,
                    std::string* err) {
    if (t.userId.empty()) {
        *err = "no broadcaster id (call FetchUser first)";
        return false;
    }
    HttpResponse r;
    const std::string headers =
        "Authorization: Bearer " + t.access + "\r\nClient-Id: " + clientId + "\r\n";
    const std::wstring path = L"/helix/streams/key?broadcaster_id=" +
                              std::wstring(t.userId.begin(), t.userId.end());
    if (!HttpRequest(L"api.twitch.tv", path.c_str(), L"GET", "", headers, &r, err)) return false;
    if (r.status != 200) {
        *err = "stream key request failed (HTTP " + std::to_string(r.status) + ")";
        return false;
    }
    if (!JsonStr(r.body, "stream_key", key)) {
        *err = "stream key response missing field";
        return false;
    }
    return true;
}

}  // namespace Twitch
