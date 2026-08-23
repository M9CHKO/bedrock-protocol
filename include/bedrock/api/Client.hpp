#pragma once

#include <bedrock/Options.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protodef/ProtoDefJson.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <iostream>
#include <filesystem>
#if defined(__linux__) || defined(__CYGWIN__) || defined(__MSYS__)
#include <unistd.h>
#endif
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
#include <windows.h>
#endif

namespace bedrock::api {

enum class DebugMode { Off, Events, Packets, Json, Trace };

struct ClientOptions {
    // Same idea as bedrock-protocol createClient({...}).
    std::string host = "localhost";
    uint16_t port = 19132;
    std::string username = "Bot";
    std::string version = std::string(CURRENT_VERSION);
    bool offline = false;

    // Xbox cache profile. Empty means: use username.
    std::string profile;

    // Bot-level flags. They replace command-line flags for library users.
    DebugMode debug = DebugMode::Off;
    bool decodePackets = false;
    bool packetDump = false;
    bool regenLogin = false;
    bool quiet = true;
    int holdSeconds = 0; // 0 means keep the connection open until the process exits.
    std::vector<std::string> extraFlags;

    // Directory that contains the installed bedrock runtime helper.
    // For external bots this is normally your build dir or install bin dir.
    std::string executableDir;

    // Optional direct path to the helper. When empty, executableDir + "/bedrock" is used.
    std::string runtimeExecutable;

    // Reserved for future internal connect timeout. Currently session prints progress itself.
    int startupTimeoutSeconds = 0;
};

struct Packet {
    uint32_t id = 0;
    std::string name;
    bool ok = true;
    std::map<std::string, std::string> fields;

    bool has(const std::string& key) const {
        return fields.find(key) != fields.end();
    }

    std::string get(const std::string& key) const {
        auto it = fields.find(key);
        return it == fields.end() ? std::string() : it->second;
    }

    std::string operator[](const std::string& key) const {
        return get(key);
    }

    bool hasField(const std::string& key) const {
        return fields.find(key) != fields.end();
    }

    bool empty(const std::string& key) const {
        return get(key).empty();
    }

    std::map<std::string, std::string> select(const std::string& prefix) const {
        std::map<std::string, std::string> out;

        for (const auto& [k, v] : fields) {
            if (k == prefix || k.rfind(prefix + ".", 0) == 0 || k.rfind(prefix + "[", 0) == 0) {
                out[k] = v;
            }
        }

        return out;
    }

    bool foldedByDefault(const std::string& key) const {
        if (name == "start_game" && key.rfind("itemstates[", 0) == 0) {
            return true;
        }
        return false;
    }

    void print(std::ostream& os = std::cout) const {
        os << "\n[PACKET] " << name << " id=" << id
           << " ok=" << (ok ? "true" : "false")
           << " fields=" << fields.size()
           << "\n";

        std::map<std::string, std::size_t> folded;

        for (const auto& [k, v] : fields) {
            if (!k.empty() && k[0] == '$') {
                continue;
            }

            if (k.find(".$") != std::string::npos) {
                continue;
            }

            if (v.rfind("<no_branch:", 0) == 0) {
                continue;
            }

            if (foldedByDefault(k)) {
                folded["itemstates"]++;
                continue;
            }

            os << "  " << k << " = " << v << "\n";
        }

        for (const auto& [k, count] : folded) {
            os << "  " << k << " = <folded " << count
               << " fields; use packet.print(\"" << k << "\")>\n";
        }
    }

    void print(const std::string& prefix, std::ostream& os = std::cout) const {
        printOnly(prefix, os);
    }

    void printOnly(const std::string& prefix, std::ostream& os = std::cout) const {
        os << "\n[PACKET] " << name << " id=" << id
           << " prefix=" << prefix << "\n";

        for (const auto& [k, v] : select(prefix)) {
            os << "  " << k << " = " << v << "\n";
        }
    }
};

struct TextPacket {
    std::string sourceName;
    std::string message;
    std::string xuid;
    std::string platformChatId;
};

class Client {
public:
    using PacketHandler = std::function<void(const Packet&)>;
    using TextHandler = std::function<void(const TextPacket&)>;

    explicit Client(ClientOptions options)
        : options_(std::move(options)) {}

    // Like bedrock-protocol: client.on("packet", ...), client.on("start_game", ...)
    void on(const std::string& packetName, PacketHandler handler) {
        if (packetName == "packet") {
            anyHandlers_.push_back(std::move(handler));
            return;
        }
        handlers_[packetName].push_back(std::move(handler));
    }

    void onAny(PacketHandler handler) {
        anyHandlers_.push_back(std::move(handler));
    }

    void onText(TextHandler handler) {
        textHandlers_.push_back(std::move(handler));
    }

    // Bedrock-protocol-like API.
    void queue(const std::string& packetName, ProtoDefValue value) {
        queuedValues_.push_back({packetName, std::move(value)});
    }

    void send(const std::string& packetName, ProtoDefValue value) {
        queue(packetName, value);
        if (!commandFilePath_.empty()) {
            appendRuntimeCommand(packetName, value);
        }
    }

    void write(const std::string& packetName, ProtoDefValue value) {
        send(packetName, std::move(value));
    }

    const auto& queuedPacketValues() const {
        return queuedValues_;
    }

    int connect() {
        normalizeOptions();

        if (!ProtocolDefinition::supportsVersion(options_.version)) {
            std::cerr << "unsupported Bedrock version: " << options_.version << "\n";
            std::cerr << "available versions:";
            for (const auto& v : ProtocolDefinition::versions()) std::cerr << " " << v;
            std::cerr << "\n";
            return 2;
        }

        setProcessEnv("BEDROCK_API_DUMP", "1");
        if (!options_.decodePackets) {
            setProcessEnv("BEDROCK_API_FAST", "1");
        } else {
            setProcessEnv("BEDROCK_API_FAST", "0");
        }
        commandFilePath_ = makeTempCommandFilePath();
        {
            std::ofstream commandFile(commandFilePath_, std::ios::binary | std::ios::trunc);
            if (!commandFile) {
                throw std::runtime_error("failed to create runtime command file: " + commandFilePath_);
            }
        }

        std::string cmd = buildCommand();
        FILE* pipe = openPipe(cmd);
        if (!pipe) {
            cleanupCommandFile();
            throw std::runtime_error("failed to start bedrock");
        }

        char buffer[8192];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            trimLine(line);
            if (line.rfind("[API] packet", 0) == 0) {
                Packet packet = parseApiLine(line);
                emit(packet);
            } else if (options_.debug != DebugMode::Off || line.rfind("[ERROR]", 0) == 0 || line.rfind("[WARN]", 0) == 0) {
                std::cout << line << "\n";
            }
        }

        int rc = closePipe(pipe);
        cleanupCommandFile();
        if (rc != 0 && options_.debug != DebugMode::Off) {
            std::cerr << "[ERROR] bedrock helper stopped before a clean exit. Check version/server or run with another supported version.\n";
        }
        return rc;
    }

    int run() {
        return connect();
    }

private:
    ClientOptions options_;
    std::unordered_map<std::string, std::vector<PacketHandler>> handlers_;
    std::vector<PacketHandler> anyHandlers_;
    std::vector<TextHandler> textHandlers_;
    std::vector<std::pair<std::string, ProtoDefValue>> queuedValues_;
    std::string commandFilePath_;

    void normalizeOptions() {
        (void) validateVersion(options_.version);
        if (options_.profile.empty()) {
            options_.profile = options_.username.empty() ? std::string("Bot") : options_.username;
        }
        if (options_.offline && options_.username.empty()) {
            options_.username = options_.profile;
        }
    }

    std::string buildCommand() const {
        std::ostringstream cmd;
        std::string dir = options_.executableDir;
        if (dir.empty()) {
            if (const char* env = std::getenv("BEDROCK_PROTOCOL_BIN_DIR")) {
                if (*env) dir = env;
            }
        }
        if (dir.empty()) {
            auto detected = currentExecutableDir();
            if (!detected.empty() && std::filesystem::exists(executablePath(detected, "bedrock"))) {
                dir = detected;
            } else {
                dir = ".";
            }
        }

        const std::string runtime = options_.runtimeExecutable.empty()
            ? executablePath(dir, "bedrock")
            : options_.runtimeExecutable;

        cmd << shellQuote(runtime) << " "
            << shellQuote(options_.version) << " "
            << shellQuote(options_.host) << " "
            << options_.port << " "
            << shellQuote(options_.profile) << " ";

        if (options_.holdSeconds > 0) cmd << options_.holdSeconds;
        else cmd << "--hold";

        if (options_.offline) cmd << " --offline";
        if (options_.regenLogin) cmd << " --regen-login";
        if (options_.packetDump) cmd << " --packet-dump";
        if (options_.decodePackets || options_.debug == DebugMode::Json) cmd << " --packet-json";
        if (options_.debug == DebugMode::Trace) cmd << " --trace";
        else if (options_.debug == DebugMode::Events || options_.debug == DebugMode::Packets) cmd << " --debug";
        else if (options_.quiet) cmd << " --quiet";
        if (!commandFilePath_.empty()) cmd << " --command-file " << shellQuote(commandFilePath_);

        for (const auto& flag : options_.extraFlags) {
            cmd << " " << flag;
        }

        cmd << " 2>&1";
        return cmd.str();
    }

    static std::string currentExecutableDir() {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
        char buf[4096];
        DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            std::string path(buf, n);
            auto slash = path.find_last_of("/\\");
            if (slash != std::string::npos) return path.substr(0, slash);
        }
#endif
#if defined(__linux__) || defined(__CYGWIN__) || defined(__MSYS__)
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            std::string path(buf);
            auto slash = path.find_last_of('/');
            if (slash != std::string::npos) return path.substr(0, slash);
        }
#endif
        return {};
    }

    static std::string executablePath(const std::string& dir, const std::string& name) {
        std::filesystem::path path = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(path)) return path.string();

#if defined(_WIN32) || defined(__CYGWIN__) || defined(__MSYS__)
        std::filesystem::path exePath = std::filesystem::path(dir) / (name + ".exe");
        if (std::filesystem::exists(exePath)) return exePath.string();
#endif

        if (dir == ".") {
            return name;
        }

        return path.string();
    }

    static std::string makeTempCommandFilePath() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = std::filesystem::temp_directory_path()
            / ("bedrock-protocol-cpp-api-" + std::to_string(stamp) + ".commands");
        return path.string();
    }

    void cleanupCommandFile() {
        if (!commandFilePath_.empty()) {
            std::error_code ec;
            std::filesystem::remove(commandFilePath_, ec);
            commandFilePath_.clear();
        }
    }

    static std::string escapeCommandValue(const std::string& input) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : input) {
            if (c == '%' || c == '\t' || c == '\n' || c == '\r' || c == '=') {
                out.push_back('%');
                out.push_back(hex[(c >> 4) & 0x0f]);
                out.push_back(hex[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    void appendRuntimeCommand(
        const std::string& packetName,
        const ProtoDefValue& value
    ) const {
        std::ofstream out(commandFilePath_, std::ios::binary | std::ios::app);
        if (!out) {
            return;
        }

        out << "send_json\t"
            << escapeCommandValue(packetName)
            << "\t"
            << escapeCommandValue(protoDefValueToJson(value));
        out << "\n";
    }

    static std::string shellQuote(const std::string& s) {
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

    static void setProcessEnv(const char* key, const char* value) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
        _putenv_s(key, value);
#else
        setenv(key, value, 1);
#endif
    }

    static FILE* openPipe(const std::string& command) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
        return _popen(command.c_str(), "r");
#else
        return popen(command.c_str(), "r");
#endif
    }

    static int closePipe(FILE* pipe) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
        return _pclose(pipe);
#else
        return pclose(pipe);
#endif
    }

    static void trimLine(std::string& s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    }

    static std::string percentDecode(const std::string& value) {
        std::string out;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%' && i + 2 < value.size()) {
                int a = hexValue(value[i + 1]);
                int b = hexValue(value[i + 2]);
                if (a >= 0 && b >= 0) {
                    out.push_back(static_cast<char>((a << 4) | b));
                    i += 2;
                    continue;
                }
            }
            out.push_back(value[i]);
        }
        return out;
    }

    static Packet parseApiLine(const std::string& line) {
        Packet p;
        std::istringstream iss(line);
        std::string token;
        iss >> token; // [API]
        iss >> token; // packet

        while (iss >> token) {
            auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            std::string key = token.substr(0, eq);
            std::string value = percentDecode(token.substr(eq + 1));

            if (key == "id") {
                p.id = static_cast<uint32_t>(std::stoul(value));
            } else if (key == "name") {
                p.name = value;
            } else if (key == "ok") {
                p.ok = (value == "true");
            } else {
                p.fields[key] = value;
            }
        }

        return p;
    }

    void emit(const Packet& packet) {
        for (auto& h : anyHandlers_) h(packet);

        auto it = handlers_.find(packet.name);
        if (it != handlers_.end()) {
            for (auto& h : it->second) h(packet);
        }

        if (packet.name == "text") {
            TextPacket text;
            text.sourceName = packet.get("source_name");
            text.message = packet.get("message");
            text.xuid = packet.get("xuid");
            text.platformChatId = packet.get("platform_chat_id");
            for (auto& h : textHandlers_) h(text);
        }
    }
};

inline Client createClient(ClientOptions options) {
    return Client(std::move(options));
}

} // namespace bedrock::api
