#pragma once
#include <cstdint>
#include <string>

// Twitch OAuth device-code flow (public client: no secret, no redirect) plus
// the Helix calls needed to turn a token into a stream key.
//
// Every function here blocks on network I/O -- call them from a worker thread.
namespace Twitch {

struct Tokens {
    std::string access;
    std::string refresh;
    int64_t expiresAtUnix = 0;
    std::string userId;
    std::string login;
    std::string displayName;
    bool valid() const { return !access.empty(); }
};

struct DeviceCode {
    std::string deviceCode;
    std::string userCode;
    std::string verificationUri;
    int interval = 5;
    int expiresIn = 1800;
};

// Scopes we ask for: reading the broadcaster's stream key is the whole point.
inline const char* kScopes = "channel:read:stream_key";

// Ulticast's registered public client (no secret is involved: this is the
// device-code flow, exactly like other desktop apps embed their client id).
inline constexpr const char* kDefaultClientId = "ocfqt946cu8dcxh18cc39q2zsnhkao";

// POST /oauth2/device -> user code + verification URI.
bool RequestDeviceCode(const std::string& clientId, DeviceCode* out, std::string* err);

enum class Poll { Pending, SlowDown, Authorized, Expired, Denied, Error };

// POST /oauth2/token (device grant). Poll until it stops returning Pending.
Poll PollToken(const std::string& clientId, const DeviceCode& code, Tokens* out, std::string* err);

// POST /oauth2/token (refresh grant).
bool Refresh(const std::string& clientId, const std::string& refreshToken, Tokens* out,
             std::string* err);

// GET /helix/users -> fills userId/login/displayName (also validates the token).
bool FetchUser(const std::string& clientId, Tokens* t, std::string* err);

// GET /helix/streams/key -> the RTMP stream key.
bool FetchStreamKey(const std::string& clientId, const Tokens& t, std::string* key,
                    std::string* err);

}  // namespace Twitch
