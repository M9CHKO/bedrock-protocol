#include <bedrock/auth/XboxLiveAuth.hpp>

#include <bedrock/BedrockKeyExchange.hpp>
#include <bedrock/LoginPacket.hpp>
#include <bedrock/auth/BedrockClientDataBuilder.hpp>
#include <bedrock/auth/MsalCachePlugin.hpp>
#include <bedrock/auth/UuidV3.hpp>
#include <bedrock/auth/XboxProfileCache.hpp>
#include <bedrock/auth/XboxTokenCache.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>
#include <bedrock/world/MinecraftDataPathResolver.hpp>

#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
#include <process.h>
#endif

namespace bedrock {
namespace {

inline constexpr std::string_view BedrockProtocolPublicKey =
    "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAECRXueJeTDqNRRgJi/vlRufByu/"
    "2G0i2Ebt6YMar5QX/R0DIIyrJMcUpruK4QveTfJSTp3Shlq4Gk34cD/"
    "4GUWwkv0DVuzeuB+tXija7HBxii03NHDbPAD0AKnLr2wdAp";

void sanitizeStdbufEnvironment() {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
    _putenv_s("LD_PRELOAD", "");
    _putenv_s("_STDBUF_O", "");
    _putenv_s("_STDBUF_E", "");
    _putenv_s("_STDBUF_I", "");
#else
    unsetenv("LD_PRELOAD");
    unsetenv("_STDBUF_O");
    unsetenv("_STDBUF_E");
    unsetenv("_STDBUF_I");
#endif
}

std::string shellQuote(const std::string& s) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

FILE* openPipe(const std::string& command) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
    return _popen(command.c_str(), "r");
#else
    return popen(command.c_str(), "r");
#endif
}

int closePipe(FILE* pipe) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

std::string execRead(const std::string& cmd) {
    std::array<char, 4096> buf {};
    std::string out;
    FILE* pipe = openPipe(cmd);
    if (!pipe) {
        throw std::runtime_error("failed to start command: " + cmd);
    }
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    int rc = closePipe(pipe);
    if (rc != 0) {
        throw std::runtime_error("command failed: " + cmd);
    }
    return out;
}

void log(const XboxLiveAuthOptions& options, const std::string& message) {
    if (options.onLog) {
        options.onLog(message);
    }
}

std::string escapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string jsonString(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    auto p = json.find(marker);
    if (p == std::string::npos) return "";
    p = json.find(':', p);
    if (p == std::string::npos) return "";
    p = json.find('"', p);
    if (p == std::string::npos) return "";

    std::string out;
    bool escaped = false;
    for (++p; p < json.size(); ++p) {
        char c = json[p];
        if (escaped) {
            out += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

int jsonInt(const std::string& json, const std::string& key, int fallback) {
    const std::string marker = "\"" + key + "\"";
    auto p = json.find(marker);
    if (p == std::string::npos) return fallback;
    p = json.find(':', p);
    if (p == std::string::npos) return fallback;
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    auto e = p;
    while (e < json.size() && std::isdigit(static_cast<unsigned char>(json[e]))) ++e;
    if (e == p) return fallback;
    return std::stoi(json.substr(p, e - p));
}

std::vector<std::string> jsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> out;
    const std::string marker = "\"" + key + "\"";
    auto p = json.find(marker);
    if (p == std::string::npos) return out;
    p = json.find('[', p);
    if (p == std::string::npos) return out;
    ++p;

    while (p < json.size()) {
        while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
        if (p >= json.size() || json[p] == ']') break;
        if (json[p] != '"') {
            ++p;
            continue;
        }

        std::string value;
        bool escaped = false;
        for (++p; p < json.size(); ++p) {
            char c = json[p];
            if (escaped) {
                value += c;
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                ++p;
                break;
            }
            value += c;
        }
        out.push_back(value);
        while (p < json.size() && json[p] != ',' && json[p] != ']') ++p;
        if (p < json.size() && json[p] == ',') ++p;
    }
    return out;
}

std::string extractDisplayClaim(const std::string& json, const std::string& key) {
    auto xui = json.find("\"xui\"");
    if (xui == std::string::npos) return "";
    return jsonString(json.substr(xui), key);
}

bool jsTruthy(const MsalConfigPtr& value) {
    return value && value->truthy();
}

std::string curlPostForm(const std::string& url, const std::string& data) {
    std::string cmd =
        "curl -sS -X POST "
        "-H 'Content-Type: application/x-www-form-urlencoded' "
        "--data " + shellQuote(data) + " " + shellQuote(url);
    return execRead(cmd);
}

std::string curlPostJson(const std::string& url, const std::string& json) {
    auto tmp = std::filesystem::temp_directory_path() / "bedrock_protocol_cpp_post.json";
    {
        std::ofstream out(tmp);
        out << json;
    }
    std::string cmd =
        "curl -sS -X POST "
        "-H 'Content-Type: application/json' "
        "--data @" + shellQuote(tmp.string()) + " " + shellQuote(url);
    auto result = execRead(cmd);
    std::filesystem::remove(tmp);
    return result;
}

std::string curlPostJsonAuth(
    const std::string& url,
    const std::string& auth,
    const std::string& json
) {
    auto tmp = std::filesystem::temp_directory_path() / "bedrock_protocol_cpp_auth_post.json";
    {
        std::ofstream out(tmp);
        out << json;
    }
    std::string cmd =
        "curl -sS -X POST "
        "-H 'Content-Type: application/json' "
        "-H " + shellQuote("Authorization: " + auth) + " "
        "--data @" + shellQuote(tmp.string()) + " " + shellQuote(url);
    auto result = execRead(cmd);
    std::filesystem::remove(tmp);
    return result;
}

std::vector<int> parseVersionParts(const std::string& version) {
    std::vector<int> out;
    std::string current;
    for (char c : version) {
        if (c == '.') {
            out.push_back(current.empty() ? 0 : std::stoi(current));
            current.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            current += c;
        }
    }
    out.push_back(current.empty() ? 0 : std::stoi(current));
    while (out.size() < 3) out.push_back(0);
    return out;
}

bool versionAtLeast(const std::string& version, int a, int b, int c) {
    auto v = parseVersionParts(version);
    if (v[0] != a) return v[0] > a;
    if (v[1] != b) return v[1] > b;
    return v[2] >= c;
}

bool jsonHasKey(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}

bool removeJsonKeyValue(std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    auto keyPos = json.find(marker);
    if (keyPos == std::string::npos) return false;
    auto valueStart = json.find(':', keyPos);
    if (valueStart == std::string::npos) return false;
    ++valueStart;

    bool inString = false;
    bool escaped = false;
    int depthObj = 0;
    int depthArr = 0;
    std::size_t end = valueStart;
    for (; end < json.size(); ++end) {
        char ch = json[end];
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') { inString = true; continue; }
        if (ch == '{') { ++depthObj; continue; }
        if (ch == '}') {
            if (depthObj == 0 && depthArr == 0) break;
            --depthObj;
            continue;
        }
        if (ch == '[') { ++depthArr; continue; }
        if (ch == ']') { --depthArr; continue; }
        if (ch == ',' && depthObj == 0 && depthArr == 0) break;
    }

    std::size_t eraseBegin = keyPos;
    std::size_t eraseEnd = end;
    if (eraseEnd < json.size() && json[eraseEnd] == ',') {
        ++eraseEnd;
    } else {
        while (eraseBegin > 0 && std::isspace(static_cast<unsigned char>(json[eraseBegin - 1]))) --eraseBegin;
        if (eraseBegin > 0 && json[eraseBegin - 1] == ',') --eraseBegin;
    }
    json.erase(eraseBegin, eraseEnd - eraseBegin);
    return true;
}

void replaceJsonStringValue(std::string& json, const std::string& key, const std::string& value) {
    auto k = json.find("\"" + key + "\"");
    if (k == std::string::npos) return;
    auto c = json.find(':', k);
    if (c == std::string::npos) return;
    auto q1 = json.find('"', c);
    if (q1 == std::string::npos) return;

    bool escaped = false;
    for (auto q2 = q1 + 1; q2 < json.size(); ++q2) {
        char ch = json[q2];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            json.replace(q1 + 1, q2 - q1 - 1, escapeJson(value));
            return;
        }
    }
}

bool replaceJsonIntValue(std::string& json, const std::string& key, long long value) {
    const std::string marker = "\"" + key + "\"";
    auto p = json.find(marker);
    if (p == std::string::npos) return false;
    p = json.find(':', p);
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
    auto e = p;
    while (e < json.size() && (json[e] == '-' || std::isdigit(static_cast<unsigned char>(json[e])))) ++e;
    json.replace(p, e - p, std::to_string(value));
    return true;
}

void addJsonRawBeforeKey(
    std::string& json,
    const std::string& beforeKey,
    const std::string& key,
    const std::string& rawValue
) {
    if (jsonHasKey(json, key)) return;
    const std::string field = "\"" + key + "\":" + rawValue;
    auto p = json.find("\"" + beforeKey + "\"");
    if (p != std::string::npos) {
        json.insert(p, field + ",");
        return;
    }
    auto brace = json.rfind('}');
    if (brace == std::string::npos) return;
    auto beforeBrace = brace;
    while (beforeBrace > 0 && std::isspace(static_cast<unsigned char>(json[beforeBrace - 1]))) --beforeBrace;
    const bool emptyObject = beforeBrace > 0 && json[beforeBrace - 1] == '{';
    json.insert(brace, (emptyObject ? "" : ",") + field);
}

void setJsonStringValue(
    std::string& json,
    const std::string& beforeKey,
    const std::string& key,
    const std::string& value
) {
    if (jsonHasKey(json, key)) {
        replaceJsonStringValue(json, key, value);
    } else {
        addJsonRawBeforeKey(json, beforeKey, key, "\"" + escapeJson(value) + "\"");
    }
}

void setJsonIntValue(
    std::string& json,
    const std::string& beforeKey,
    const std::string& key,
    long long value
) {
    if (!replaceJsonIntValue(json, key, value)) {
        addJsonRawBeforeKey(json, beforeKey, key, std::to_string(value));
    }
}

void setJsonBoolValue(
    std::string& json,
    const std::string& beforeKey,
    const std::string& key,
    bool value
) {
    removeJsonKeyValue(json, key);
    addJsonRawBeforeKey(json, beforeKey, key, value ? "true" : "false");
}

std::string makeHex16() {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::string s;
    s.reserve(16);
    for (int i = 0; i < 16; ++i) {
        s.push_back(hex[rd() & 15]);
    }
    return s;
}

void normalizeClientDataForVersion(std::string& payload, const std::string& version) {
    if (!versionAtLeast(version, 1, 19, 20)) removeJsonKeyValue(payload, "TrustedSkin");
    if (!versionAtLeast(version, 1, 19, 62)) removeJsonKeyValue(payload, "OverrideSkin");
    if (!versionAtLeast(version, 1, 19, 80)) removeJsonKeyValue(payload, "CompatibleWithClientSideChunkGen");
    if (!versionAtLeast(version, 1, 21, 42)) {
        removeJsonKeyValue(payload, "MaxViewDistance");
        removeJsonKeyValue(payload, "MemoryTier");
        removeJsonKeyValue(payload, "PlatformType");
    }
    if (!versionAtLeast(version, 1, 21, 90)) {
        addJsonRawBeforeKey(payload, "UIProfile", "ThirdPartyNameOnly", "false");
    } else {
        removeJsonKeyValue(payload, "ThirdPartyNameOnly");
    }

    const auto nowMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    replaceJsonIntValue(payload, "ClientRandomId", nowMs);
    replaceJsonStringValue(payload, "PlayFabId", makeHex16());
}

void applyRuntimeClientDataFields(
    std::string& payload,
    const std::string& displayName,
    const std::string& version,
    const std::string& selfSignedUuid,
    const std::string& serverAddress
) {
    const std::string beforeKey = "SkinAnimationData";
    setJsonStringValue(payload, beforeKey, "ThirdPartyName", displayName);
    setJsonStringValue(payload, beforeKey, "GameVersion", version);
    setJsonStringValue(payload, beforeKey, "DeviceId", selfSignedUuid);
    setJsonStringValue(payload, beforeKey, "SelfSignedId", selfSignedUuid);
    setJsonStringValue(payload, beforeKey, "ServerAddress", serverAddress);
    setJsonStringValue(payload, beforeKey, "DeviceModel", "PrismarineJS");
    setJsonStringValue(payload, beforeKey, "LanguageCode", "en_GB");
    setJsonStringValue(payload, beforeKey, "PlatformOfflineId", "");
    setJsonStringValue(payload, beforeKey, "PlatformOnlineId", "");
    setJsonStringValue(payload, beforeKey, "PlayFabId", makeHex16());

    const auto nowMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    setJsonIntValue(payload, beforeKey, "ClientRandomId", nowMs);
    setJsonIntValue(payload, beforeKey, "CurrentInputMode", 1);
    setJsonIntValue(payload, beforeKey, "DefaultInputMode", 1);
    setJsonIntValue(payload, beforeKey, "DeviceOS", 7);
    setJsonIntValue(payload, beforeKey, "GraphicsMode", 1);
    setJsonIntValue(payload, beforeKey, "GuiScale", -1);
    setJsonIntValue(payload, beforeKey, "UIProfile", 0);

    setJsonBoolValue(payload, beforeKey, "IsEditorMode", false);
    if (versionAtLeast(version, 1, 19, 20)) setJsonBoolValue(payload, beforeKey, "TrustedSkin", false);
    if (versionAtLeast(version, 1, 19, 62)) setJsonBoolValue(payload, beforeKey, "OverrideSkin", false);
    if (versionAtLeast(version, 1, 19, 80)) setJsonBoolValue(payload, beforeKey, "CompatibleWithClientSideChunkGen", false);
    if (versionAtLeast(version, 1, 21, 42)) {
        setJsonIntValue(payload, beforeKey, "MaxViewDistance", 0);
        setJsonIntValue(payload, beforeKey, "MemoryTier", 0);
        setJsonIntValue(payload, beforeKey, "PlatformType", 0);
    }
    normalizeClientDataForVersion(payload, version);
}

std::string readTextIfExists(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return "";
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string readVersionedSteveClientData(
    const std::string& version,
    const std::vector<std::filesystem::path>& minecraftDataRoots,
    std::string& source
) {
    for (const auto& root : minecraftDataRoots) {
        const auto dataPaths = root / "dataPaths.json";
        if (!std::filesystem::exists(dataPaths)) continue;
        try {
            MinecraftDataPathResolver resolver(dataPaths);
            auto paths = resolver.findBedrock(version);
            if (!paths || paths->steve.empty()) continue;
            const auto steveJson = root / "bedrock" / paths->steve / "steve.json";
            auto payload = readTextIfExists(steveJson);
            if (!payload.empty()) {
                source = steveJson.string();
                return payload;
            }
        } catch (const std::exception&) {
        }
    }
    return "";
}

std::string jwtHeaderString(const std::string& jwt, const std::string& key) {
    try {
        auto header = BedrockKeyExchange::extractJwtHeaderJson(jwt);
        return jsonString(header, key);
    } catch (const std::exception&) {
        return "";
    }
}

std::string extractChainExtraDataString(
    const std::vector<std::string>& chain,
    const std::string& key
) {
    for (const auto& jwt : chain) {
        try {
            auto payload = BedrockKeyExchange::extractJwtPayloadJson(jwt);
            auto extra = payload.find("\"extraData\"");
            if (extra == std::string::npos) continue;
            auto value = jsonString(payload.substr(extra), key);
            if (!value.empty()) return value;
        } catch (const std::exception&) {
        }
    }
    return "";
}

std::vector<std::filesystem::path> defaultMinecraftDataRoots(
    const XboxLiveAuthOptions& options
) {
    std::vector<std::filesystem::path> roots = options.minecraftDataRoots;
    if (const char* env = std::getenv("BEDROCK_DATA_DIR")) {
        if (*env) {
            const auto dataDir = std::filesystem::path(env);
            roots.push_back(dataDir / "minecraft-data");
            roots.push_back(dataDir);
        }
    }
    roots.push_back(std::filesystem::current_path() / "data" / "minecraft-data");
    roots.push_back(std::filesystem::current_path().parent_path() / "data" / "minecraft-data");
    roots.push_back(std::filesystem::current_path().parent_path() / "share" / "bedrock-protocol-cpp" / "data" / "minecraft-data");
    return roots;
}

std::string buildClientData(
    const XboxLiveAuthOptions& options,
    const std::string& displayName,
    const std::string& xuid,
    const std::string& selfSignedUuid
) {
    if (!options.clientDataJson.empty()) {
        auto payload = options.clientDataJson;
        setJsonStringValue(payload, "SkinAnimationData", "ServerAddress", options.serverAddress);
        normalizeClientDataForVersion(payload, options.version);
        log(options, "clientData source=forwarded");
        return payload;
    }

    std::string source;
    auto payload = readVersionedSteveClientData(
        options.version,
        defaultMinecraftDataRoots(options),
        source
    );

    if (!payload.empty()) {
        applyRuntimeClientDataFields(
            payload,
            displayName,
            options.version,
            selfSignedUuid,
            options.serverAddress
        );
        log(options, "clientData source=" + source);
        return payload;
    }

    BedrockClientDataOptions clientDataOptions;
    clientDataOptions.displayName = displayName;
    clientDataOptions.xuid = xuid;
    clientDataOptions.gameVersion = options.version;
    clientDataOptions.deviceId = selfSignedUuid;
    clientDataOptions.serverAddress = options.serverAddress;
    payload = BedrockClientDataBuilder::buildClassicSkinClientData(clientDataOptions);
    normalizeClientDataForVersion(payload, options.version);
    log(options, "clientData source=generated");
    return payload;
}

XboxTokenCacheData runDeviceCodeLogin(
    const XboxLiveAuthOptions& options,
    const XboxProfileCachePaths& paths
) {
    if (!options.interactiveAuth) {
        throw std::runtime_error("Xbox auth cache missing for profile=" + options.profileName);
    }

    // LiveTokenManager receives authTitle as its OAuth client id. Options are
    // normalized before this path, but resolve again here so this network edge
    // cannot silently regress to the former MinecraftJava id.
    const auto authFlow = XboxLiveAuth::resolveFlowOptions(options);
    const std::string& clientId = authFlow.authTitle;

    auto deviceResp = curlPostForm(
        "https://login.live.com/oauth20_connect.srf",
        "client_id=" + clientId +
        "&scope=service::user.auth.xboxlive.com::MBI_SSL"
        "&response_type=device_code"
    );

    const std::string deviceCode = jsonString(deviceResp, "device_code");
    const std::string userCode = jsonString(deviceResp, "user_code");
    const std::string verificationUri = jsonString(deviceResp, "verification_uri");
    const std::string message = jsonString(deviceResp, "message");
    int interval = jsonInt(deviceResp, "interval", 5);

    if (deviceCode.empty()) {
        throw std::runtime_error("failed to get Xbox device code: " + deviceResp);
    }

    XboxDeviceCodeInfo info;
    info.verificationUri = verificationUri;
    info.userCode = userCode;
    info.message = message;
    if (options.onMsaCode) {
        options.onMsaCode(info);
    } else if (options.onDeviceCode) {
        options.onDeviceCode(info);
    } else {
        log(options, "Open " + verificationUri + " and enter code " + userCode);
    }

    std::string msAccess;
    std::string msRefresh;
    std::string msExpires;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        auto tokenResp = curlPostForm(
            "https://login.live.com/oauth20_token.srf?client_id=" + clientId,
            "grant_type=urn:ietf:params:oauth:grant-type:device_code"
            "&client_id=" + clientId +
            "&device_code=" + deviceCode
        );

        const std::string err = jsonString(tokenResp, "error");
        if (err == "authorization_pending") {
            log(options, "waiting for Xbox login...");
            continue;
        }
        if (err == "slow_down") {
            interval += 5;
            continue;
        }
        if (!err.empty()) {
            throw std::runtime_error("Microsoft token error: " + err + " " + tokenResp);
        }

        msAccess = jsonString(tokenResp, "access_token");
        msRefresh = jsonString(tokenResp, "refresh_token");
        msExpires = "expires_in:" + std::to_string(jsonInt(tokenResp, "expires_in", 0));
        break;
    }

    auto xblResp = curlPostJson(
        "https://user.auth.xboxlive.com/user/authenticate",
        "{"
        "\"Properties\":{"
        "\"AuthMethod\":\"RPS\","
        "\"SiteName\":\"user.auth.xboxlive.com\","
        "\"RpsTicket\":\"t=" + escapeJson(msAccess) + "\""
        "},"
        "\"RelyingParty\":\"http://auth.xboxlive.com\","
        "\"TokenType\":\"JWT\""
        "}"
    );
    const std::string xblToken = jsonString(xblResp, "Token");
    if (xblToken.empty()) {
        throw std::runtime_error("XBL user token failed: " + xblResp);
    }

    auto xstsResp = curlPostJson(
        "https://xsts.auth.xboxlive.com/xsts/authorize",
        "{"
        "\"Properties\":{"
        "\"SandboxId\":\"RETAIL\","
        "\"UserTokens\":[\"" + escapeJson(xblToken) + "\"]"
        "},"
        "\"RelyingParty\":\"rp://api.minecraftservices.com/\","
        "\"TokenType\":\"JWT\""
        "}"
    );
    const std::string xstsToken = jsonString(xstsResp, "Token");
    if (xstsToken.empty()) {
        throw std::runtime_error("XSTS failed: " + xstsResp);
    }

    XboxTokenCacheData data;
    data.profileName = options.profileName;
    data.gamertag = extractDisplayClaim(xstsResp, "gtg");
    data.xuid = extractDisplayClaim(xstsResp, "xid");
    data.microsoftAccessToken = msAccess;
    data.microsoftRefreshToken = msRefresh;
    data.microsoftExpiresAt = msExpires;
    data.xboxUserToken = xblToken;
    data.xstsToken = xstsToken;
    data.xstsUserHash = extractDisplayClaim(xstsResp, "uhs");
    XboxTokenCacheFile::save(paths.authJson, data);
    return data;
}

XboxTokenCacheData loadOrLoginXboxCache(
    const XboxLiveAuthOptions& options,
    const XboxProfileCachePaths& paths
) {
    if (std::filesystem::exists(paths.authJson)) {
        return XboxTokenCacheFile::load(paths.authJson);
    }
    return runDeviceCodeLogin(options, paths);
}

std::vector<std::string> requestBedrockChain(
    const XboxLiveAuthOptions& options,
    const XboxTokenCacheData& cache,
    const BedrockClientKeyPair& keys
) {
    if (cache.xboxUserToken.empty()) {
        throw std::runtime_error("Xbox auth cache is missing xboxUserToken");
    }

    auto xstsResp = curlPostJson(
        "https://xsts.auth.xboxlive.com/xsts/authorize",
        "{"
        "\"Properties\":{"
        "\"SandboxId\":\"RETAIL\","
        "\"UserTokens\":[\"" + escapeJson(cache.xboxUserToken) + "\"]"
        "},"
        "\"RelyingParty\":\"https://multiplayer.minecraft.net/\","
        "\"TokenType\":\"JWT\""
        "}"
    );

    const std::string token = jsonString(xstsResp, "Token");
    const std::string uhs = extractDisplayClaim(xstsResp, "uhs");
    if (token.empty() || uhs.empty()) {
        throw std::runtime_error("Bedrock XSTS failed: " + xstsResp);
    }

    const std::string authHeader = "XBL3.0 x=" + uhs + ";" + token;
    auto authResp = curlPostJsonAuth(
        "https://multiplayer.minecraft.net/authentication",
        authHeader,
        "{\"identityPublicKey\":\"" + escapeJson(keys.publicKeyDerBase64) + "\"}"
    );

    auto chain = jsonStringArray(authResp, "chain");
    if (chain.empty()) {
        throw std::runtime_error("multiplayer authentication returned no chain: " + authResp);
    }
    return chain;
}

XboxLiveLoginPacket buildLoginPacket(
    const XboxLiveAuthOptions& options,
    BedrockClientKeyPair keys,
    std::vector<std::string> chain,
    std::string identity,
    std::string displayName,
    std::string xuid,
    std::optional<std::string> onlineIdentityPublicKey = std::nullopt
) {
    auto accessToken = chain;
    const auto clientPayload = buildClientData(
        options,
        displayName,
        xuid,
        identity
    );
    const auto clientJwt = BedrockAuthJwt::signEs384Jwt(
        keys.privateKeyPem,
        keys.publicKeyDerBase64,
        clientPayload
    );

    const auto nowSec = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    std::string clientToRootPayload;
    if (options.offline) {
        clientToRootPayload =
            "{"
            "\"extraData\":{"
            "\"displayName\":\"" + escapeJson(displayName) + "\","
            "\"identity\":\"" + escapeJson(identity) + "\","
            "\"titleId\":\"89692877\","
            "\"XUID\":\"0\""
            "},"
            "\"certificateAuthority\":true,"
            "\"identityPublicKey\":\"" + escapeJson(keys.publicKeyDerBase64) + "\","
            "\"iat\":" + std::to_string(nowSec) +
            "}";
    } else {
        const std::string rootPublicKey = onlineIdentityPublicKey.has_value()
            ? *onlineIdentityPublicKey
            : jwtHeaderString(chain.front(), "x5u");
        if (rootPublicKey.empty()) {
            throw std::runtime_error("Bedrock chain first JWT has no header.x5u");
        }
        clientToRootPayload =
            "{"
            "\"identityPublicKey\":\"" + escapeJson(rootPublicKey) + "\","
            "\"certificateAuthority\":true,"
            "\"iat\":" + std::to_string(nowSec) +
            "}";
    }

    auto clientToRootJwt = BedrockAuthJwt::signEs384Jwt(
        keys.privateKeyPem,
        keys.publicKeyDerBase64,
        clientToRootPayload
    );
    chain.insert(chain.begin(), std::move(clientToRootJwt));

    std::string certificateJson = "{\"chain\":[";
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (i) certificateJson += ",";
        certificateJson += "\"" + escapeJson(chain[i]) + "\"";
    }
    certificateJson += "]}";

    std::string identityJson;
    if (versionAtLeast(options.version, 1, 21, 90)) {
        identityJson =
            "{"
            "\"Certificate\":\"" + escapeJson(certificateJson) + "\","
            "\"AuthenticationType\":" + std::string(options.offline ? "2" : "0") + ","
            "\"Token\":\"\""
            "}";
    } else {
        identityJson = std::move(certificateJson);
    }

    XboxLiveLoginPacket out;
    out.loginPacket = LoginPacketCodec::encode(
        options.protocolVersion,
        identityJson,
        clientJwt
    );
    out.keyPair = std::move(keys);
    out.accessToken = std::move(accessToken);
    out.identity = std::move(identity);
    out.displayName = std::move(displayName);
    out.xuid = std::move(xuid);
    out.online = !options.offline;
    return out;
}

struct AuthflowProfile {
    std::string identity;
    std::string displayName;
    std::string xuid;
};

AuthflowProfile extractAuthflowProfile(
    const std::vector<std::string>& chains
) {
    // auth.js does not search the chain: the Xbox profile is always decoded
    // from chains[1]. Preserve V8's observable missing-element failure before
    // any packet construction or cache work.
    if (chains.size() < 2) {
        throw std::runtime_error(
            "Cannot read properties of undefined (reading 'split')"
        );
    }

    const auto payload =
        BedrockKeyExchange::extractJwtPayloadJson(chains[1]);
    const auto xboxProfile = ProtoDefJson::parse(payload);
    const auto* extraData = xboxProfile.kind == ProtoDefValue::Kind::Object
        ? xboxProfile.get("extraData")
        : nullptr;
    const auto* profile = extraData &&
            extraData->kind == ProtoDefValue::Kind::Object
        ? extraData
        : nullptr;
    const auto claim = [profile](const char* name) {
        const auto* value = profile ? profile->get(name) : nullptr;
        return value && value->kind == ProtoDefValue::Kind::String
            ? value->stringValue
            : std::string{};
    };

    AuthflowProfile result;
    result.displayName = claim("displayName");
    result.identity = claim("identity");
    result.xuid = claim("XUID");

    // auth.js uses JavaScript || for each property independently.
    if (result.displayName.empty()) result.displayName = "Player";
    if (result.identity.empty()) {
        result.identity = "adfcf5ca-206c-404a-aec4-f59fff264c9b";
    }
    if (result.xuid.empty()) result.xuid = "0";
    return result;
}

XboxLiveAuthOptions normalizeOptions(
    XboxLiveAuthOptions options,
    bool prismarineAuthFlowPrepared = false
) {
    if (options.profileName.empty()) options.profileName = "Bot";
    // options.js always derives this value from Versions; callers cannot
    // override it independently of the selected Minecraft version.
    options.protocolVersion = validateVersion(options.version);
    const auto authFlow = XboxLiveAuth::resolveFlowOptions(options);
    options.authTitle = authFlow.authTitle;
    options.deviceType = authFlow.deviceType;
    options.flow = authFlow.flow;
    if (!options.offline) {
        XboxLiveAuth::validatePrismarineAuthFlowPresence(authFlow);
        if (!prismarineAuthFlowPrepared) {
            options.msalConfig = XboxLiveAuth::initializePrismarineAuthFlow(
                authFlow,
                options.msalConfig
            );
        }
    }
    return options;
}

} // namespace

XboxLiveAuthFlowOptions XboxLiveAuth::resolveFlowOptions(
    const XboxLiveAuthOptions& options
) {
    XboxLiveAuthFlowOptions resolved;
    if (options.authTitle.has_value()) {
        resolved.authTitle = *options.authTitle;
        resolved.deviceType = options.deviceType;
        resolved.flow = options.flow;
    } else if (options.authTitle.isNull()) {
        // validateOptions only defaults a property whose value is undefined.
        // null remains public and flows through PrismarineAuth as a falsy
        // authTitle without receiving Nintendo/live defaults.
        resolved.authTitle.clear();
        resolved.deviceType = options.deviceType;
        resolved.flow = options.flow;
    } else if (!options.xboxClientId.empty()) {
        resolved.authTitle = options.xboxClientId;
        resolved.deviceType = options.deviceType;
        resolved.flow = options.flow;
    } else {
        resolved.authTitle = std::string(Titles::MinecraftNintendoSwitch);
        // auth.js assigns these two values only in the same branch where
        // authTitle is undefined; supplied values are overwritten there.
        resolved.deviceType = "Nintendo";
        resolved.flow = "live";
    }
    return resolved;
}

void XboxLiveAuth::validatePrismarineAuthFlowPresence(
    const XboxLiveAuthFlowOptions& options
) {
    if (options.flow.empty()) {
        throw std::runtime_error(
            "Missing 'flow' argument in options. See docs for more information."
        );
    }
}

void XboxLiveAuth::validatePrismarineAuthFlow(
    const XboxLiveAuthFlowOptions& options
) {
    if (options.flow == "live" || options.flow == "sisu") {
        if (options.authTitle.empty()) {
            throw std::runtime_error(
                "Please specify an \"authTitle\" in Authflow constructor when using " +
                options.flow + " flow"
            );
        }
        return;
    }

    if (options.flow == "msal") {
        if (options.authTitle.empty()) {
            throw std::runtime_error(
                "Must specify an Azure client ID token inside the `authTitle` parameter "
                "when using Azure-based auth. See "
                "https://learn.microsoft.com/en-us/entra/identity-platform/"
                "quickstart-register-app#register-an-application for more information "
                "on obtaining an Azure token."
            );
        }
        return;
    }

    throw std::runtime_error(
        "Unknown flow: " + options.flow +
        " (expected \"live\", \"sisu\", or \"msal\")"
    );
}

MsalConfigPtr XboxLiveAuth::initializePrismarineAuthFlow(
    const XboxLiveAuthFlowOptions& options,
    const MsalConfigPtr& msalConfig
) {
    return initializePrismarineAuthFlow(options, msalConfig, {});
}

MsalConfigPtr XboxLiveAuth::initializePrismarineAuthFlow(
    const XboxLiveAuthFlowOptions& options,
    const MsalConfigPtr& msalConfig,
    AuthCachePtr msaCache
) {
    return initializePrismarineAuthFlowRuntime(
        options,
        msalConfig,
        std::move(msaCache)
    ).effectiveMsalConfig;
}

PrismarineAuthFlowRuntime XboxLiveAuth::
initializePrismarineAuthFlowRuntime(
    const XboxLiveAuthFlowOptions& options,
    const MsalConfigPtr& msalConfig,
    AuthCachePtr msaCache,
    MsalPublicClientApplicationFactory applicationFactory,
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue,
    MsaTokenManagerObservers observers,
    LiveTokenManagerDependencies liveDependencies
) {
    validatePrismarineAuthFlowPresence(options);

    if (options.flow == "live" || options.flow == "sisu") {
        validatePrismarineAuthFlow(options);
        if (!liveDependencies.microtaskQueue) {
            liveDependencies.microtaskQueue = std::move(microtaskQueue);
        }
        auto manager = std::make_shared<LiveTokenManager>(
            JsRuntimeValue::string(options.authTitle),
            JsRuntimeValue::array({
                JsRuntimeValue::string(
                    "service::user.auth.xboxlive.com::MBI_SSL"
                )
            }),
            std::move(msaCache),
            std::move(liveDependencies)
        );
        return PrismarineAuthFlowRuntime {
            .live = std::move(manager),
            .doTitleAuth = true
        };
    }

    if (options.flow != "msal") {
        validatePrismarineAuthFlow(options);
        return {};
    }

    MsalConfigPtr effective;
    if (jsTruthy(msalConfig)) {
        // JavaScript keeps the exact supplied object. In particular it does
        // not clone or publish a normalized replacement.
        effective = msalConfig;
    } else {
        // The Azure client-id error is conditional on a falsy msalConfig.
        validatePrismarineAuthFlow(options);
        effective = makeMsalConfig(options.authTitle);
    }

    auto manager = std::make_shared<MsaTokenManager>(
        effective,
        JsRuntimeValue::array({
            JsRuntimeValue::string("XboxLive.signin"),
            JsRuntimeValue::string("offline_access")
        }),
        std::move(msaCache),
        std::move(applicationFactory),
        std::move(microtaskQueue),
        std::move(observers)
    );
    return PrismarineAuthFlowRuntime {
        .effectiveMsalConfig = std::move(effective),
        .msa = std::move(manager),
        .doTitleAuth = false
    };
}

bool XboxLiveAuth::isTruthyMsalConfig(const MsalConfigPtr& msalConfig) {
    return jsTruthy(msalConfig);
}

BedrockClientKeyPair XboxLiveAuth::loadOrCreateProfileKeyPair(
    const std::string& profileName,
    const std::filesystem::path& cacheRoot
) {
    XboxProfileCache profiles(cacheRoot.empty() ? XboxProfileCache() : XboxProfileCache(cacheRoot));
    profiles.ensureProfileDir(profileName);
    auto paths = profiles.pathsFor(profileName);
    return BedrockAuthJwt::loadOrCreateKeyPair(
        paths.profileDir / "client_private_key.pem",
        paths.profileDir / "client_public_key.der.b64"
    );
}

XboxLiveLoginPacket XboxLiveAuth::makeLoginPacket(XboxLiveAuthOptions options) {
    sanitizeStdbufEnvironment();
    options = normalizeOptions(std::move(options));

    XboxProfileCache profiles(options.cacheRoot.empty() ? XboxProfileCache() : XboxProfileCache(options.cacheRoot));
    profiles.ensureProfileVersionDir(options.profileName, options.version);
    auto paths = profiles.pathsForVersion(options.profileName, options.version);

    auto keys = BedrockAuthJwt::loadOrCreateKeyPair(
        paths.profileDir / "client_private_key.pem",
        paths.profileDir / "client_public_key.der.b64"
    );

    XboxTokenCacheData cache;
    std::vector<std::string> chain;
    // auth.js createOfflineSession() derives this directly from username via
    // uuid-1345 v3 and never stores it in prismarine-auth's profile cache.
    std::string identity = options.offline
        ? uuidFrom(options.profileName)
        : std::string{};
    std::string displayName = options.profileName;
    std::string xuid = "0";

    if (!options.offline) {
        cache = loadOrLoginXboxCache(options, paths);
        chain = requestBedrockChain(options, cache, keys);
        identity = extractChainExtraDataString(chain, "identity");
        displayName = extractChainExtraDataString(chain, "displayName");
        xuid = extractChainExtraDataString(chain, "XUID");
        if (displayName.empty()) displayName = !cache.minecraftName.empty() ? cache.minecraftName : cache.gamertag;
        if (xuid.empty()) xuid = cache.xuid;
        if (identity.empty()) {
            throw std::runtime_error("Bedrock authentication chain has no extraData.identity");
        }
    }

    if (displayName.empty()) displayName = options.profileName;
    if (xuid.empty()) xuid = options.offline ? "0" : cache.xuid;
    return buildLoginPacket(
        options,
        std::move(keys),
        std::move(chain),
        std::move(identity),
        std::move(displayName),
        std::move(xuid),
        std::string(BedrockProtocolPublicKey)
    );
}

XboxLiveLoginPacket XboxLiveAuth::makeLoginPacketFromPreparedFlow(
    XboxLiveAuthOptions options,
    MsalConfigPtr effectiveMsalConfig
) {
    sanitizeStdbufEnvironment();
    options.msalConfig = std::move(effectiveMsalConfig);
    options = normalizeOptions(std::move(options), true);

    XboxProfileCache profiles(options.cacheRoot.empty() ? XboxProfileCache() : XboxProfileCache(options.cacheRoot));
    profiles.ensureProfileVersionDir(options.profileName, options.version);
    auto paths = profiles.pathsForVersion(options.profileName, options.version);

    auto keys = BedrockAuthJwt::loadOrCreateKeyPair(
        paths.profileDir / "client_private_key.pem",
        paths.profileDir / "client_public_key.der.b64"
    );

    XboxTokenCacheData cache;
    std::vector<std::string> chain;
    std::string identity = options.offline
        ? uuidFrom(options.profileName)
        : std::string{};
    std::string displayName = options.profileName;
    std::string xuid = "0";

    if (!options.offline) {
        cache = loadOrLoginXboxCache(options, paths);
        chain = requestBedrockChain(options, cache, keys);
        identity = extractChainExtraDataString(chain, "identity");
        displayName = extractChainExtraDataString(chain, "displayName");
        xuid = extractChainExtraDataString(chain, "XUID");
        if (displayName.empty()) displayName = !cache.minecraftName.empty() ? cache.minecraftName : cache.gamertag;
        if (xuid.empty()) xuid = cache.xuid;
        if (identity.empty()) {
            throw std::runtime_error("Bedrock authentication chain has no extraData.identity");
        }
    }

    if (displayName.empty()) displayName = options.profileName;
    if (xuid.empty()) xuid = options.offline ? "0" : cache.xuid;
    return buildLoginPacket(
        options,
        std::move(keys),
        std::move(chain),
        std::move(identity),
        std::move(displayName),
        std::move(xuid),
        std::string(BedrockProtocolPublicKey)
    );
}

XboxLiveLoginPacket XboxLiveAuth::makeLoginPacketFromChains(
    XboxLiveAuthOptions options,
    BedrockClientKeyPair keyPair,
    std::vector<std::string> chains
) {
    // The normal Client/Options boundary has already validated the selected
    // protocol version before options.authflow is invoked. Keep this public
    // auth-layer entry point self-contained without running normalizeOptions,
    // whose flow checks belong exclusively to `new PrismarineAuth(...)`.
    if (options.profileName.empty()) options.profileName = "Bot";
    options.protocolVersion = validateVersion(options.version);
    options.offline = false;

    auto profile = extractAuthflowProfile(chains);
    // A supplied Authflow owns its chain. Deriving the root from chains[0]
    // keeps the normal Xbox chain identical (its x5u is Mojang's key) while
    // allowing deterministic private-CA integration tests and custom
    // Authflow implementations to remain cryptographically self-consistent.
    return buildLoginPacket(
        options,
        std::move(keyPair),
        std::move(chains),
        std::move(profile.identity),
        std::move(profile.displayName),
        std::move(profile.xuid)
    );
}

} // namespace bedrock
