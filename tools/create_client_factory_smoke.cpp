#include <bedrock/bedrock.hpp>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bedrock {

struct ClientFactoryTestAccess {
    static void setAfterNetworkConnectHook(
        Client& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        client.state_->afterNetworkConnectTestHook = std::move(hook);
    }

    static bool connectWorkerExited(const Client& client) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        return client.state_->connectWorkerExited;
    }

    static void recordUnhandledAsyncError(
        Client& client,
        const std::string& message
    ) {
        Client::recordUnhandledAsyncError(client.state_, message, true);
    }

    static void markAutoConnect(Client& client) {
        Client::markAutoConnect(client.state_);
    }

    static void launchAutoConnectWorker(Client& client) {
        Client::launchAutoConnectWorker(client.state_);
    }

    static bool autoConnectCancelled(const Client& client) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        return client.state_->autoConnectCancelled;
    }

    static bool autoConnectFinished(const Client& client) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        return client.state_->autoConnectFinished;
    }

    static bool deferredClose(const Client& client) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        return client.state_->deferredClose;
    }

    static void setAfterWorkerDecisionHook(
        Client& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        client.state_->afterWorkerDecisionTestHook = std::move(hook);
    }

    static std::weak_ptr<void> weakState(const Client& client) {
        return client.state_;
    }

    static std::shared_ptr<BedrockNetworkClient> sharedNetwork(
        const Client& client
    ) {
        std::lock_guard<std::mutex> lock(client.state_->mutex);
        return client.state_->network;
    }

    static void installNoCloseFinalizingWorker(
        Client& client,
        std::atomic<bool>& entered,
        std::atomic<bool>& release
    ) {
        auto state = client.state_;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->connectWorkerStarted = true;
            state->autoConnectFinished = true;
            state->connectWorkerFinalization =
                Client::State::ConnectWorkerFinalization::NoClose;
        }
        std::lock_guard<std::mutex> workersLock(state->workersMutex);
        state->connectThread = std::thread([state, &entered, &release]() {
            entered = true;
            while (!release.load()) std::this_thread::yield();
            std::lock_guard<std::mutex> lock(state->mutex);
            state->connectWorkerFinalization =
                Client::State::ConnectWorkerFinalization::Exited;
            state->connectWorkerExited = true;
        });
    }
};

struct BedrockNetworkClientTestAccess {
    static void emitError(BedrockNetworkClient& client, const std::string& message) {
        client.emitError(message);
    }

    static void emitStatus(
        BedrockNetworkClient& client,
        BedrockNetworkClientStatus status
    ) {
        client.setStatus(status);
    }

    static void emitStatusAsRakNetCallback(
        BedrockNetworkClient& client,
        BedrockNetworkClientStatus status
    ) {
        const auto provider = client.callbackLifetimeProviderSnapshot();
        auto lifetimeLease = provider
            ? provider()
            : std::shared_ptr<void>();
        client.enterRakNetCallback();
        try {
            client.setStatus(status);
        } catch (...) {
            client.leaveRakNetCallback();
            throw;
        }
        client.leaveRakNetCallback();
    }

    static void emitTransportClose(
        BedrockNetworkClient& client,
        const std::string& reason
    ) {
        client.emitClose(
            reason,
            false,
            BedrockNetworkClient::CloseOrigin::Transport
        );
    }

    static void clearHandlers(BedrockNetworkClient& client) {
        client.clearEventHandlers();
    }

    static void enterRakNetCallback(BedrockNetworkClient& client) {
        client.enterRakNetCallback();
    }

    static void leaveRakNetCallback(BedrockNetworkClient& client) {
        client.leaveRakNetCallback();
    }

    static void handleConnected(BedrockNetworkClient& client) {
        client.handleRakNetConnected();
    }

    static std::size_t queuedPacketCount(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.queueMutex_);
        return client.queuedPackets_.size();
    }

    static bool queueRunning(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.queueMutex_);
        return !client.stopQueue_;
    }

    static bool queuePumpEnabled(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.queueMutex_);
        return client.queuePumpEnabled_;
    }

    static void installIdleTransport(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.sendMutex_);
        client.raknet_ = std::make_shared<RakNetClient>();
    }

    static bool hasTransport(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.sendMutex_);
        return static_cast<bool>(client.raknet_);
    }

    static std::filesystem::path effectiveAuthenticationCacheRoot(
        BedrockNetworkClient& client
    ) {
        std::lock_guard<std::mutex> lock(client.optionsMutex_);
        return client.authenticationCacheRoot_;
    }

    static bool stopRequested(const BedrockNetworkClient& client) {
        return client.rakNetStopRequested_.load();
    }

    static bool closing(const BedrockNetworkClient& client) {
        return client.closing_.load();
    }

    static void setAfterTransportInstallHook(
        BedrockNetworkClient& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.eventHandlersMutex_);
        client.afterTransportInstallTestHook_ = std::move(hook);
    }

    static void setBeforeTransportInstallHook(
        BedrockNetworkClient& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.eventHandlersMutex_);
        client.beforeTransportInstallTestHook_ = std::move(hook);
    }

    static void setBeforeConnectPhaseCommitHook(
        BedrockNetworkClient& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.eventHandlersMutex_);
        client.beforeConnectPhaseCommitTestHook_ = std::move(hook);
    }

    static bool connectLifecycleActive(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.connectLifecycleMutex_);
        return client.connectLifecyclePhase_ ==
            BedrockNetworkClient::ConnectLifecyclePhase::Active;
    }

    static bool connectLifecycleIdle(BedrockNetworkClient& client) {
        std::lock_guard<std::mutex> lock(client.connectLifecycleMutex_);
        return client.connectLifecyclePhase_ ==
            BedrockNetworkClient::ConnectLifecyclePhase::Idle;
    }

    static void setBeforeQueueStartHook(
        BedrockNetworkClient& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.eventHandlersMutex_);
        client.beforeQueueStartTestHook_ = std::move(hook);
    }
};

struct RakNetClientTestAccess {
    static void dispatch(RakNetClient& client, const std::vector<uint8_t>& packet) {
        client.handlePacket(packet);
    }

    static void setBeforeRunningCommitHook(
        RakNetClient& client,
        std::function<void()> hook
    ) {
        std::lock_guard<std::mutex> lock(client.callbackMutex_);
        client.beforeRunningCommitTestHook_ = std::move(hook);
    }
};

} // namespace bedrock

namespace {

using namespace std::chrono_literals;

constexpr uint8_t kMagic[16] = {
    0x00, 0xff, 0xff, 0x00,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfd, 0xfd, 0xfd, 0xfd,
    0x12, 0x34, 0x56, 0x78
};

void writeU64BE(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

std::vector<uint8_t> prefixedAdvertisement(const std::string& advertisement) {
    const auto length = static_cast<uint16_t>(advertisement.size());
    std::vector<uint8_t> out {
        static_cast<uint8_t>((length >> 8u) & 0xffu),
        static_cast<uint8_t>(length & 0xffu)
    };
    out.insert(out.end(), advertisement.begin(), advertisement.end());
    return out;
}

std::string advertisement(const std::string& version, uint16_t portV4) {
    return "MCPE;Factory Motd;924;" + version +
        ";0;5;42;Factory Level;Creative;1;" +
        std::to_string(portV4) + ";19133;0;";
}

std::vector<uint8_t> makePong(
    const std::vector<uint8_t>& request,
    const std::string& version,
    uint16_t portV4
) {
    std::vector<uint8_t> out {0x1c};
    if (request.size() >= 9) {
        out.insert(out.end(), request.begin() + 1, request.begin() + 9);
    } else {
        out.resize(9, 0);
    }
    writeU64BE(out, 0x0102030405060708ULL);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    const auto payload = prefixedAdvertisement(advertisement(version, portV4));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> connectedDatagram(
    uint32_t sequence,
    std::vector<uint8_t> payload
) {
    const auto bitLength = static_cast<uint16_t>(payload.size() * 8u);
    std::vector<uint8_t> out {
        0x80,
        static_cast<uint8_t>(sequence & 0xffu),
        static_cast<uint8_t>((sequence >> 8u) & 0xffu),
        static_cast<uint8_t>((sequence >> 16u) & 0xffu),
        0x00,
        static_cast<uint8_t>((bitLength >> 8u) & 0xffu),
        static_cast<uint8_t>(bitLength & 0xffu)
    };
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

class PingResponder {
public:
    PingResponder(
        std::string version,
        uint16_t advertisedPort,
        std::chrono::milliseconds replyDelay = 25ms
    ) : version_(std::move(version)),
        advertisedPort_(advertisedPort),
        replyDelay_(replyDelay) {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ < 0) throw std::runtime_error("socket failed");

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            close(socket_);
            throw std::runtime_error("bind failed");
        }
        socklen_t length = sizeof(address);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            close(socket_);
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);

        worker_ = std::thread([this]() { run(); });
    }

    PingResponder(const PingResponder&) = delete;
    PingResponder& operator=(const PingResponder&) = delete;

    ~PingResponder() {
        finish();
        if (socket_ >= 0) close(socket_);
    }

    uint16_t port() const noexcept { return port_; }
    bool receivedPing() const noexcept { return receivedPing_.load(); }
    const std::string& error() const noexcept { return error_; }
    void finish() {
        if (worker_.joinable()) worker_.join();
    }

private:
    int socket_ = -1;
    uint16_t port_ = 0;
    std::string version_;
    uint16_t advertisedPort_ = 0;
    std::chrono::milliseconds replyDelay_;
    std::thread worker_;
    std::atomic<bool> receivedPing_ {false};
    std::string error_;

    void run() {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket_, &readSet);
        timeval timeout {};
        timeout.tv_sec = 2;
        if (select(socket_ + 1, &readSet, nullptr, nullptr, &timeout) <= 0) {
            error_ = "did not receive ping";
            return;
        }

        std::vector<uint8_t> request(2048);
        sockaddr_storage source {};
        socklen_t sourceLength = sizeof(source);
        const auto received = recvfrom(
            socket_, request.data(), request.size(), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength
        );
        if (received <= 0) {
            error_ = "recvfrom failed";
            return;
        }
        request.resize(static_cast<std::size_t>(received));
        if (request.empty() || request[0] != 0x01) {
            error_ = "first datagram was not ID_UNCONNECTED_PING";
            return;
        }
        receivedPing_ = true;
        std::this_thread::sleep_for(replyDelay_);

        const auto pong = makePong(request, version_, advertisedPort_);
        const auto sent = sendto(
            socket_, pong.data(), pong.size(), 0,
            reinterpret_cast<const sockaddr*>(&source), sourceLength
        );
        if (sent < 0 || static_cast<std::size_t>(sent) != pong.size()) {
            error_ = "sendto failed";
        }
        // Keep the socket open but deliberately do not answer RakNet open
        // requests. This detects a blocking internal connect_allowed listener.
    }
};

bool check(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "[CREATE-CLIENT-FACTORY-SMOKE] " << message << "\n";
    return false;
}

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 1500ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

bedrock::Options baseOptions(uint16_t port) {
    bedrock::Options options;
    options.host = "127.0.0.1";
    options.port = port;
    options.username = "FactorySmoke";
    options.offline = true;
    options.connectTimeout = 35;
    options.conLog = {};
    return options;
}

bool checkDirectAndSkipPing() {
    bool ok = true;
    bedrock::Options raw;
    ok &= check(raw.host.empty(), "raw Options injected a JS-missing host");
    ok &= check(raw.username.empty(), "raw Options injected a JS-missing username");
    ok &= check(raw.version.empty(), "raw omitted version sentinel changed");
    ok &= check(!raw.version.provided(),
                "default Options incorrectly marked version as present");
    ok &= check(!raw.port.has_value() && !raw.port.provided(),
                "raw Options injected the createClient-only port default");
    ok &= check(!raw.followPort.has_value(), "direct raw followPort must be absent");

    const std::string emptyVersion;
    bedrock::RequestedVersion assignedRange;
    assignedRange.assign(emptyVersion.begin(), emptyVersion.end());
    bedrock::RequestedVersion appended;
    appended.append("");
    bedrock::RequestedVersion inserted;
    inserted.insert(0, "");
    bedrock::RequestedVersion replaced;
    replaced.replace(0, 0, "");
    bedrock::RequestedVersion added;
    added += "";
    bedrock::RequestedVersion swapped;
    std::string emptySwap;
    swapped.swap(emptySwap);
    bedrock::RequestedVersion swapOmitted;
    bedrock::RequestedVersion swapExplicit("1.20.40");
    swapOmitted.swap(swapExplicit);
    ok &= check(
        assignedRange.provided() && appended.provided() &&
            inserted.provided() && replaced.provided() &&
            added.provided() && swapped.provided() &&
            swapOmitted.provided() && swapExplicit.provided() &&
            swapOmitted == "1.20.40" && swapExplicit.empty(),
        "empty-preserving RequestedVersion mutator lost property presence"
    );

    bool rawHostExact = false;
    try {
        bedrock::Client invalidRaw(raw);
        (void) invalidRaw;
    } catch (const std::exception& error) {
        rawHostExact = std::string(error.what()) == "Invalid host/port";
    }
    ok &= check(rawHostExact,
                "direct Client with omitted host did not reject exactly");

    bool rawPortExact = false;
    try {
        bedrock::Options missingPort;
        missingPort.host = "127.0.0.1";
        missingPort.username = "FactorySmoke";
        missingPort.offline = true;
        missingPort.conLog = {};
        bedrock::Client invalidRaw(missingPort);
        (void) invalidRaw;
    } catch (const std::exception& error) {
        rawPortExact = std::string(error.what()) == "Invalid host/port";
    }
    ok &= check(rawPortExact,
                "direct Client with omitted port did not reject exactly");

    bedrock::Options delayedOptions;
    delayedOptions.delayedInit = true;
    bedrock::Client delayed(std::move(delayedOptions));
    ok &= check(!delayed.initialized() &&
                    delayed.options().delayedInit.truthy(),
                "delayedInit=true did not defer direct Client.init");
    delayed.options().host = "127.0.0.1";
    delayed.options().port = 0;
    delayed.options().username = "DelayedFactorySmoke";
    delayed.options().offline = true;
    delayed.options().connectTimeout = 0;
    delayed.options().conLog = {};
    delayed.init();
    ok &= check(delayed.initialized() &&
                    delayed.options().port == 0 &&
                    delayed.options().connectTimeout == std::optional<int>(0) &&
                    delayed.network().options().connectTimeoutMs == 9000,
                "mutable delayed options were not consumed by init()");
    delayed.close();

    bool delayedInitErrorExact = false;
    try {
        bedrock::Options invalidDelayedOptions;
        invalidDelayedOptions.delayedInit = true;
        bedrock::Client invalidDelayed(std::move(invalidDelayedOptions));
        invalidDelayed.init();
    } catch (const std::exception& error) {
        delayedInitErrorExact = std::string(error.what()) == "Invalid host/port";
    }
    ok &= check(delayedInitErrorExact,
                "public delayed init() did not retain JS validation boundary");

    std::vector<std::string> delayedPingLogs;
    bedrock::Options delayedPingOptions;
    delayedPingOptions.host = "delayed-host";
    delayedPingOptions.port = 19132;
    delayedPingOptions.delayedInit = true;
    delayedPingOptions.conLog = [&](const std::string& message) {
        delayedPingLogs.push_back(message);
    };
    bedrock::Client delayedPing(std::move(delayedPingOptions));
    bool delayedPingErrorExact = false;
    try {
        (void)delayedPing.ping();
    } catch (const std::exception& error) {
        delayedPingErrorExact = std::string(error.what()) ==
            "Cannot read properties of undefined (reading 'ping')";
    }
    ok &= check(
        delayedPingErrorExact && delayedPingLogs == std::vector<std::string>{
            "Unable to connect to [delayed-host]/19132. Is the server running?"
        },
        "Client::ping before init did not match the JS catch/log boundary"
    );

    PingResponder directPingResponder("1.20.40", 0, 0ms);
    auto directPingOptions = baseOptions(directPingResponder.port());
    bedrock::Client directPingClient(directPingOptions);
    const auto directPong = directPingClient.ping();
    ok &= check(directPong.size() > 7 && directPong.substr(2, 5) == "MCPE;",
                "Client::ping did not return the raw RakNet advertisement");
    directPingResponder.finish();

    auto undefinedTimeoutOptions = baseOptions(9);
    undefinedTimeoutOptions.connectTimeout = bedrock::jsUndefined;
    bedrock::Client undefinedTimeout(undefinedTimeoutOptions);
    ok &= check(undefinedTimeout.options().connectTimeout.isUndefined() &&
                    undefinedTimeout.options().connectTimeout.hasOwn() &&
                    undefinedTimeout.network().options().connectTimeoutMs == 9000,
                "explicit connectTimeout=undefined did not override defaultOptions");

    auto nullTimeoutOptions = baseOptions(9);
    nullTimeoutOptions.connectTimeout = nullptr;
    bedrock::Client nullTimeout(nullTimeoutOptions);
    ok &= check(nullTimeout.options().connectTimeout.isNull() &&
                    nullTimeout.options().connectTimeout.hasOwn() &&
                    nullTimeout.network().options().connectTimeoutMs == 9000,
                "explicit connectTimeout=null did not retain null/|| semantics");

    auto directOptions = baseOptions(9);
    directOptions.connectTimeout.reset();
    bedrock::Client direct(directOptions);
    ok &= check(direct.initialized(), "direct Client did not initialize synchronously");
    ok &= check(
        direct.options().version == std::string(bedrock::CURRENT_VERSION),
        "direct Client did not normalize omitted version to CURRENT_VERSION"
    );
    ok &= check(
        !direct.options().followPort.has_value(),
        "direct Client injected factory-only followPort"
    );
    ok &= check(
        direct.options().connectTimeout == std::optional<int>(9000),
        "direct Client did not expose defaultOptions.connectTimeout immediately"
    );
    ok &= check(!direct.autoConnectStarted(), "direct Client unexpectedly auto-connected");

    auto explicitZeroPort = baseOptions(9);
    explicitZeroPort.port = 0;
    bedrock::Client directZeroPort(explicitZeroPort);
    ok &= check(
        directZeroPort.options().port.has_value() &&
            directZeroPort.options().port.provided() &&
            directZeroPort.options().port == 0,
        "direct Client lost explicit port=0 presence"
    );

    std::atomic<int> logs {0};
    auto skipped = baseOptions(9);
    skipped.skipPing = true;
    skipped.version = "1.20.40";
    skipped.followPort = false;
    skipped.conLog = [&](const std::string&) { ++logs; };
    const auto begin = std::chrono::steady_clock::now();
    auto client = bedrock::createClient(skipped);
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    ok &= check(client.initialized(), "skipPing did not init before return");
    ok &= check(client.autoConnectStarted(), "skipPing did not launch connect before return");
    ok &= check(client.options().version == "1.20.40", "skipPing changed explicit version");
    ok &= check(client.options().port == 9, "skipPing changed port");
    ok &= check(client.options().followPort == std::optional<bool>(false),
                "skipPing lost explicit followPort=false");
    ok &= check(client.options().delayedInit.truthy() &&
                    client.options().delayedInit.hasOwn(),
                "createClient did not force delayedInit=true as its final field");
    ok &= check(logs.load() == 0, "skipPing called conLog");
    // The normal-path transport-install barrier below is the deterministic
    // proof that the native handshake runs off-thread. Keep only a generous
    // regression bound here: synchronous packet/auth preparation can exceed
    // 100 ms on a contended sanitizer or parallel compiler host.
    ok &= check(elapsed < 500ms, "skipPing waited for blocking RakNet connect");

    std::atomic<int> lateEvents {0};
    client.onConnectAllowed([&]() { ++lateEvents; });
    std::this_thread::sleep_for(15ms);
    ok &= check(lateEvents.load() == 0,
                "connect_allowed was replayed to a post-return skipPing listener");
    ok &= check(client.connect(), "duplicate factory connect was not idempotent");

    auto omitted = baseOptions(9);
    omitted.skipPing = true;
    auto omittedClient = bedrock::createClient(omitted);
    ok &= check(
        omittedClient.options().version == std::string(bedrock::CURRENT_VERSION),
        "skipPing omitted version did not use CURRENT_VERSION"
    );
    ok &= check(
        omittedClient.options().followPort == std::optional<bool>(true),
        "non-Realms factory did not inject followPort=true"
    );

    auto factoryDefaultPort = baseOptions(9);
    factoryDefaultPort.port = bedrock::RequestedPort {};
    factoryDefaultPort.skipPing = true;
    auto factoryDefaultPortClient = bedrock::createClient(factoryDefaultPort);
    ok &= check(
        factoryDefaultPortClient.options().port.has_value() &&
            factoryDefaultPortClient.options().port.hasOwn() &&
            !factoryDefaultPortClient.options().port.provided() &&
            factoryDefaultPortClient.options().port == 19132,
        "createClient did not inject its factory-only port=19132"
    );
    factoryDefaultPortClient.close();

    auto factoryZeroPort = baseOptions(9);
    factoryZeroPort.port = 0;
    factoryZeroPort.skipPing = true;
    auto factoryZeroPortClient = bedrock::createClient(factoryZeroPort);
    ok &= check(
        factoryZeroPortClient.options().port.has_value() &&
            factoryZeroPortClient.options().port.provided() &&
            factoryZeroPortClient.options().port == 0,
        "createClient replaced explicit port=0 with 19132"
    );
    factoryZeroPortClient.close();

    const auto checkNullishFactoryPort = [&](auto nullish, const std::string& label) {
        bool exact = false;
        try {
            auto options = baseOptions(9);
            options.port = nullish;
            options.skipPing = true;
            auto invalidClient = bedrock::createClient(options);
            (void)invalidClient;
        } catch (const std::exception& error) {
            exact = std::string(error.what()) == "Invalid host/port";
        }
        ok &= check(exact, label);
    };
    checkNullishFactoryPort(
        bedrock::jsUndefined,
        "createClient replaced explicit port=undefined with 19132"
    );
    checkNullishFactoryPort(
        nullptr,
        "createClient replaced explicit port=null with 19132"
    );

    auto undefinedFollow = baseOptions(9);
    undefinedFollow.skipPing = true;
    undefinedFollow.followPort = bedrock::jsUndefined;
    auto undefinedFollowClient = bedrock::createClient(undefinedFollow);
    ok &= check(undefinedFollowClient.options().followPort.isUndefined(),
                "createClient replaced explicit followPort=undefined");
    undefinedFollowClient.close();

    auto nullFollow = baseOptions(9);
    nullFollow.skipPing = true;
    nullFollow.followPort = nullptr;
    auto nullFollowClient = bedrock::createClient(nullFollow);
    ok &= check(nullFollowClient.options().followPort.isNull(),
                "createClient replaced explicit followPort=null");
    nullFollowClient.close();

    bool invalidAddressExact = false;
    try {
        auto invalid = baseOptions(19132);
        invalid.skipPing = true;
        invalid.host = "invalid host name";
        auto invalidClient = bedrock::createClient(invalid);
        (void) invalidClient;
    } catch (const std::exception& e) {
        invalidAddressExact = std::string(e.what()) ==
            "Invalid connection address invalid host name/19132";
    }
    ok &= check(invalidAddressExact,
                "skipPing invalid host did not reject synchronously/exactly");

    bool missingHostExact = false;
    try {
        auto missingHost = baseOptions(9);
        missingHost.skipPing = true;
        missingHost.host.clear();
        auto invalidClient = bedrock::createClient(missingHost);
        (void) invalidClient;
    } catch (const std::exception& error) {
        missingHostExact = std::string(error.what()) == "Invalid host/port";
    }
    ok &= check(missingHostExact,
                "skipPing omitted/empty host did not reject synchronously/exactly");

    bool explicitEmptySkipPingExact = false;
    try {
        auto explicitEmpty = baseOptions(9);
        explicitEmpty.skipPing = true;
        explicitEmpty.version = "";
        ok &= check(explicitEmpty.version.provided(),
                    "plain empty version assignment lost property presence");
        auto invalidVersion = bedrock::createClient(explicitEmpty);
        (void) invalidVersion;
    } catch (const std::exception& error) {
        explicitEmptySkipPingExact =
            std::string(error.what()) == "Unsupported version ";
    }
    ok &= check(
        explicitEmptySkipPingExact,
        "skipPing explicit empty version defaulted instead of rejecting"
    );

    auto preparationThrowOptions = baseOptions(9);
    preparationThrowOptions.offline = false;
    preparationThrowOptions.authTitle.reset();
    preparationThrowOptions.deviceType = "must-be-overwritten";
    preparationThrowOptions.flow = "must-be-overwritten";
    preparationThrowOptions.profilesFolder = false;
    bedrock::Client preparationThrow(preparationThrowOptions);
    bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
        preparationThrow.network(),
        []() { throw std::runtime_error("queue start preparation boom"); }
    );
    bool preparationThrowExact = false;
    try {
        bedrock::ClientFactoryTestAccess::markAutoConnect(preparationThrow);
    } catch (const std::exception& error) {
        preparationThrowExact =
            std::string(error.what()) == "queue start preparation boom";
    }
    bedrock::ClientFactoryTestAccess::launchAutoConnectWorker(preparationThrow);
    const auto preparationThrowResolved = preparationThrow.options();
    const auto preparationThrowCache =
        bedrock::BedrockNetworkClientTestAccess::
            effectiveAuthenticationCacheRoot(preparationThrow.network());
    ok &= check(
        preparationThrowExact && preparationThrow.autoConnectStarted() &&
            bedrock::ClientFactoryTestAccess::autoConnectCancelled(
                preparationThrow
            ) &&
            bedrock::ClientFactoryTestAccess::autoConnectFinished(
                preparationThrow
            ) &&
            !preparationThrow.connectWorkerStarted(),
        "failed synchronous auto-connect preparation launched a retry worker"
    );
    ok &= check(
        preparationThrowResolved.authTitle == std::optional<std::string>(
            std::string(bedrock::Titles::MinecraftNintendoSwitch)) &&
            preparationThrowResolved.deviceType == "Nintendo" &&
            preparationThrowResolved.flow == "live" &&
            preparationThrowResolved.profilesFolder.truthy() &&
            preparationThrowResolved.authCacheRoot.empty() &&
            preparationThrowCache ==
                preparationThrowResolved.profilesFolder.path(),
        "failed auto-connect preparation lost preceding auth.js option mutations"
    );
    return ok;
}

bool checkCanonicalClientOptionsParity() {
    bool ok = true;

    const bedrock::Options raw;
    ok &= check(!raw.connectTimeout.has_value(),
                "raw canonical connectTimeout lost omission");
    ok &= check(!raw.batchingInterval.has_value(),
                "raw canonical batchingInterval lost omission");
    ok &= check(!raw.viewDistance.has_value(),
                "raw canonical viewDistance lost omission");
    ok &= check(!raw.profilesFolder.provided(),
                "raw profilesFolder lost omission");
    ok &= check(raw.raknetBackend == "raknet-native" &&
                    raw.useRaknetWorkers &&
                    !raw.useNativeRaknet.has_value(),
                "raw RakNet defaults differ from options.js");
    ok &= check(raw.compressionAlgorithm == "deflate" &&
                    raw.compressionLevel == 7 &&
                    raw.compressionThreshold == 512,
                "raw compression defaults differ from options.js");

    const bedrock::BedrockNetworkClientOptions rawNetwork;
    ok &= check(rawNetwork.host.empty() && rawNetwork.username.empty(),
                "low-level client injected host/username defaults");

    bool missingUsernameExact = false;
    try {
        auto missingUsername = baseOptions(9);
        missingUsername.skipPing = true;
        missingUsername.username.clear();
        auto invalidClient = bedrock::createClient(missingUsername);
        (void) invalidClient;
    } catch (const std::exception& error) {
        missingUsernameExact =
            std::string(error.what()) == "Must specify a valid username";
    }
    ok &= check(missingUsernameExact,
                "offline missing username did not fail at connect boundary exactly");

    auto aliases = baseOptions(9);
    aliases.connectTimeout.reset();
    aliases.connectTimeoutMs = 321;
    aliases.batchingIntervalMs = -7;
    aliases.chunkRadius = -3;
    bedrock::Client aliasClient(aliases);
    const auto aliasPublic = aliasClient.options();
    const auto aliasNetwork = aliasClient.network().options();
    ok &= check(aliasPublic.connectTimeout == std::optional<int>(9000) &&
                    !aliasPublic.batchingInterval.has_value() &&
                    !aliasPublic.viewDistance.has_value(),
                "Client.options did not merge the canonical JS defaults");
    ok &= check(aliasPublic.connectTimeoutMs == 321 &&
                    aliasPublic.batchingIntervalMs == -7 &&
                    aliasPublic.chunkRadius == -3 &&
                    aliasNetwork.connectTimeoutMs == 9000 &&
                    aliasNetwork.batchingIntervalMs == 20 &&
                    aliasNetwork.chunkRadius == 10,
                "legacy extension fields affected canonical JS behavior");

    auto canonical = baseOptions(9);
    canonical.connectTimeoutMs = 123;
    canonical.connectTimeout = 0;
    canonical.batchingIntervalMs = 456;
    canonical.batchingInterval = -8;
    canonical.chunkRadius = 77;
    canonical.viewDistance = 0;
    canonical.profilesFolder = false;
    canonical.authCacheRoot = "legacy-cache-must-not-win-offline";
    canonical.raknetBackend = "jsp-raknet";
    canonical.useNativeRaknet = true;
    canonical.useRaknetWorkers = false;
    canonical.compressionAlgorithm = "snappy";
    canonical.compressionThreshold = 123;
    bedrock::Client canonicalClient(canonical);
    const auto canonicalPublic = canonicalClient.options();
    const auto canonicalNetwork = canonicalClient.network().options();
    ok &= check(canonicalPublic.connectTimeout == std::optional<int>(0) &&
                    canonicalPublic.connectTimeoutMs == 123 &&
                    canonicalNetwork.connectTimeoutMs == 9000,
                "connectTimeout=0 did not use JS || 9000 semantics");
    ok &= check(canonicalPublic.batchingInterval == std::optional<int>(-8) &&
                    canonicalPublic.batchingIntervalMs == 456 &&
                    canonicalNetwork.batchingIntervalMs == 1,
                "negative batchingInterval did not use Node's 1ms clamp");
    ok &= check(canonicalPublic.viewDistance == std::optional<int32_t>(0) &&
                    canonicalNetwork.viewDistance == std::optional<int32_t>(0) &&
                    canonicalPublic.chunkRadius == 77 &&
                    canonicalNetwork.chunkRadius == 10,
                "options.viewDistance incorrectly changed the live chunk radius");
    ok &= check(canonicalPublic.raknetBackend == "raknet-native" &&
                    canonicalNetwork.raknetBackend == "raknet-native" &&
                    !canonicalNetwork.useRaknetWorkers,
                "useNativeRaknet=true did not override canonical backend");
    ok &= check(canonicalNetwork.compressionAlgorithm == "snappy" &&
                    canonicalNetwork.compressionThreshold == 123,
                "canonical compression options were not preserved");
    ok &= check(!canonicalNetwork.authTitle.has_value() &&
                    canonicalNetwork.deviceType.empty() &&
                    canonicalNetwork.flow.empty() &&
                    canonicalNetwork.profilesFolder.isBoolean() &&
                    !canonicalNetwork.profilesFolder.booleanValue() &&
                    canonicalNetwork.authCacheRoot ==
                        std::filesystem::path("legacy-cache-must-not-win-offline"),
                "offline construction ran online auth option defaulting");

    auto otherTruthiness = baseOptions(9);
    otherTruthiness.connectTimeout = -20;
    otherTruthiness.batchingInterval = 0;
    otherTruthiness.viewDistance = -4;
    bedrock::Client otherTruthinessClient(otherTruthiness);
    ok &= check(otherTruthinessClient.network().options().connectTimeoutMs == 1 &&
                    otherTruthinessClient.network().options().batchingIntervalMs == 20 &&
                    otherTruthinessClient.network().options().viewDistance ==
                        std::optional<int32_t>(-4) &&
                    otherTruthinessClient.network().options().chunkRadius == 10,
                "canonical timer truthiness or upstream viewDistance bug diverged");

    auto online = baseOptions(9);
    online.offline = false;
    online.authTitle.reset();
    online.deviceType = "overwritten-by-auth-js";
    online.flow = "overwritten-by-auth-js";
    online.profilesFolder = false;
    online.authCacheRoot = "legacy-cache-must-not-win";
    bedrock::Client onlineClient(online);
    ok &= check(!onlineClient.options().authTitle.has_value() &&
                    onlineClient.options().profilesFolder.isBoolean(),
                "online auth defaults ran before connect boundary");
    bedrock::ClientFactoryTestAccess::markAutoConnect(onlineClient);
    const auto onlineResolved = onlineClient.options();
    const auto onlineEffectiveCache =
        bedrock::BedrockNetworkClientTestAccess::
            effectiveAuthenticationCacheRoot(onlineClient.network());
    ok &= check(onlineResolved.authTitle == std::optional<std::string>(
                    std::string(bedrock::Titles::MinecraftNintendoSwitch)) &&
                    onlineResolved.deviceType == "Nintendo" &&
                    onlineResolved.flow == "live",
                "online auth boundary did not apply auth.js title/device/flow defaults");
    ok &= check(onlineResolved.profilesFolder.provided() &&
                    onlineResolved.profilesFolder.truthy() &&
                    onlineResolved.authCacheRoot ==
                        std::filesystem::path("legacy-cache-must-not-win") &&
                    onlineEffectiveCache ==
                        onlineResolved.profilesFolder.path() &&
                    onlineEffectiveCache.filename() == "nmp-cache",
                "profilesFolder=false did not keep public options separate "
                "from the private online JS cache");
    onlineClient.close();

    auto directPreparationThrowOptions = baseOptions(9);
    directPreparationThrowOptions.offline = false;
    directPreparationThrowOptions.authTitle.reset();
    directPreparationThrowOptions.profilesFolder = false;
    bedrock::Client directPreparationThrow(directPreparationThrowOptions);
    bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
        directPreparationThrow.network(),
        []() { throw std::runtime_error("direct queue start preparation boom"); }
    );
    bool directPreparationThrowExact = false;
    try {
        (void) directPreparationThrow.connect();
    } catch (const std::exception& error) {
        directPreparationThrowExact =
            std::string(error.what()) == "direct queue start preparation boom";
    }
    const auto directPreparationThrowResolved = directPreparationThrow.options();
    const auto directPreparationThrowCache =
        bedrock::BedrockNetworkClientTestAccess::
            effectiveAuthenticationCacheRoot(directPreparationThrow.network());
    ok &= check(
        directPreparationThrowExact &&
            directPreparationThrowResolved.authTitle ==
                std::optional<std::string>(
                    std::string(bedrock::Titles::MinecraftNintendoSwitch)) &&
            directPreparationThrowResolved.deviceType == "Nintendo" &&
            directPreparationThrowResolved.flow == "live" &&
            directPreparationThrowResolved.profilesFolder.truthy() &&
            directPreparationThrowResolved.authCacheRoot.empty() &&
            directPreparationThrowCache ==
                directPreparationThrowResolved.profilesFolder.path(),
        "direct failed preparation lost preceding auth.js option mutations"
    );
    directPreparationThrow.close();

    auto explicitOnline = baseOptions(9);
    explicitOnline.offline = false;
    explicitOnline.authTitle = "";
    explicitOnline.deviceType = "ExplicitDevice";
    explicitOnline.flow = "ExplicitFlow";
    explicitOnline.profilesFolder = "canonical-profile-cache";
    explicitOnline.authCacheRoot = "legacy-profile-cache";
    bedrock::Client explicitOnlineClient(explicitOnline);
    bedrock::ClientFactoryTestAccess::markAutoConnect(explicitOnlineClient);
    const auto explicitResolved = explicitOnlineClient.options();
    const auto explicitEffectiveCache =
        bedrock::BedrockNetworkClientTestAccess::
            effectiveAuthenticationCacheRoot(explicitOnlineClient.network());
    ok &= check(explicitResolved.authTitle == std::optional<std::string>("") &&
                    explicitResolved.deviceType == "ExplicitDevice" &&
                    explicitResolved.flow == "ExplicitFlow",
                "explicit authTitle did not preserve deviceType/flow");
    ok &= check(
        explicitResolved.authCacheRoot ==
            std::filesystem::path("legacy-profile-cache") &&
            explicitEffectiveCache ==
                std::filesystem::path("canonical-profile-cache"),
        "canonical profilesFolder did not remain private from authCacheRoot"
    );
    explicitOnlineClient.close();

    bool unsupportedJsBackendExact = false;
    try {
        auto jsBackend = baseOptions(9);
        jsBackend.useNativeRaknet = false;
        bedrock::Client unsupported(jsBackend);
        (void) unsupported;
    } catch (const std::exception& error) {
        unsupportedJsBackendExact = std::string(error.what()) ==
            "RakNet backend is not implemented in C++: jsp-raknet";
    }
    ok &= check(unsupportedJsBackendExact,
                "jsp-raknet gap was silently mapped to the native backend");
    return ok;
}

bool checkAsyncOrderAndFourPartVersion() {
    bool ok = true;
    constexpr uint16_t kFollowedPort = 23010;
    PingResponder responder("1.20.40.9", kFollowedPort, 80ms);
    auto options = baseOptions(responder.port());
    options.connectTimeout = 250;
    options.batchingInterval = 5000;

    std::mutex logMutex;
    std::string connectionLog;
    options.conLog = [&](const std::string& value) {
        std::lock_guard<std::mutex> lock(logMutex);
        connectionLog = value;
    };

    const auto begin = std::chrono::steady_clock::now();
    auto client = bedrock::createClient(options);
    const auto returnedAfter = std::chrono::steady_clock::now() - begin;
    ok &= check(returnedAfter < 50ms, "normal factory did not return before ping");
    ok &= check(!client.initialized(), "normal factory initialized before delayed pong");
    bool earlyDisconnectReturned = true;
    try {
        client.disconnect("ignored-before-init");
    } catch (...) {
        earlyDisconnectReturned = false;
    }
    ok &= check(
        earlyDisconnectReturned && !client.initialized(),
        "disconnect before delayed init did not silently observe Disconnected"
    );
    ok &= check(
        !client.versionLessThan("1.20.40") &&
        !client.versionGreaterThan("1.20.40") &&
        !client.versionGreaterThanOrEqualTo("1.20.40") &&
        !client.versionLessThanOrEqualTo("1.20.40") &&
        !client.versionLessThan(622u) &&
        !client.versionGreaterThanOrEqualTo(622u) &&
        !client.versionLessThan("constructor"),
        "pre-init undefined protocolVersion did not make comparisons false"
    );
    bool unknownComparisonExact = false;
    try {
        (void) client.versionLessThan("not-a-version");
    } catch (const std::exception& e) {
        unknownComparisonExact =
            std::string(e.what()) == "Unknown version: not-a-version";
    }
    ok &= check(unknownComparisonExact,
                "pre-init unknown string comparison did not throw helper error");

    bool threwExact = false;
    try {
        (void) client.connect();
    } catch (const std::exception& e) {
        threwExact = std::string(e.what()) == "Connect not currently allowed";
    }
    ok &= check(threwExact, "early connect did not throw exact JS message");
    ok &= check(waitFor([&]() { return responder.receivedPing(); }, 60ms),
                "factory did not send ping before listener registration");

    std::atomic<int> errors {0};
    std::atomic<int> allowed {0};
    std::atomic<int> lateDuringEmit {0};
    std::atomic<bool> internalWasFirst {false};
    std::atomic<bool> queueRunningBeforeExternal {false};
    std::atomic<bool> externalQueuedPacketSurvived {false};
    std::atomic<bool> transportInstallHookEntered {false};
    std::atomic<bool> releaseTransportInstallHook {false};
    std::atomic<int64_t> allowedAtMs {-1};
    std::atomic<bool> stopOptionsReader {false};
    std::atomic<bool> invalidOptionsSnapshot {false};
    std::atomic<int> optionSnapshots {0};
    client.onError([&](const std::string&) { ++errors; });
    client.onConnectAllowed([&]() {
        internalWasFirst = client.autoConnectStarted();
        queueRunningBeforeExternal =
            bedrock::BedrockNetworkClientTestAccess::queueRunning(
                client.network()
            );
        client.queue(
            "client_cache_status",
            bedrock::ProtoDefValue::object({
                {"enabled", bedrock::ProtoDefValue::boolean(false)}
            })
        );
        externalQueuedPacketSurvived =
            bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                client.network()
            ) == 1;
        bedrock::BedrockNetworkClientTestAccess::setAfterTransportInstallHook(
            client.network(),
            [&]() {
                transportInstallHookEntered = true;
                while (!releaseTransportInstallHook.load()) {
                    std::this_thread::yield();
                }
            }
        );
        allowedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin
        ).count();
        ++allowed;
        client.onConnectAllowed([&]() { ++lateDuringEmit; });
    });

    std::thread optionsReader([&]() {
        while (!stopOptionsReader.load()) {
            const auto snapshot = client.optionsSnapshot();
            const bool before =
                snapshot.version == std::string(bedrock::CURRENT_VERSION) &&
                snapshot.port == responder.port();
            const bool after =
                snapshot.version == "1.20.40" &&
                snapshot.port == kFollowedPort;
            if (!before && !after) invalidOptionsSnapshot = true;
            ++optionSnapshots;
            std::this_thread::yield();
        }
    });

    ok &= check(waitFor([&]() { return allowed.load() == 1; }),
                "connect_allowed was not emitted");
    stopOptionsReader = true;
    optionsReader.join();
    ok &= check(optionSnapshots.load() > 0 && !invalidOptionsSnapshot.load(),
                "options() snapshot raced with delayed version/port mutation");
    ok &= check(internalWasFirst.load(),
                "external connect_allowed ran before internal auto-connect listener");
    ok &= check(
        queueRunningBeforeExternal.load() &&
            externalQueuedPacketSurvived.load(),
        "internal connect did not start/reset queue before external connect_allowed"
    );
    ok &= check(allowedAtMs.load() >= 60 && allowedAtMs.load() < 180,
                "connect_allowed waited for the silent RakNet handshake");
    ok &= check(lateDuringEmit.load() == 0,
                "listener added during connect_allowed joined the active snapshot");
    const bool workerReachedInstalledTransport = waitFor(
        [&]() { return transportInstallHookEntered.load(); },
        500ms
    );
    const bool queuedPacketSurvivedWorkerStart =
        bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
            client.network()
        ) == 1;
    releaseTransportInstallHook = true;
    ok &= check(
        workerReachedInstalledTransport && queuedPacketSurvivedWorkerStart,
        "blocking connect worker reset the packet queued by connect_allowed"
    );
    ok &= check(client.options().version == "1.20.40",
                "four-part advertisement was not sliced to exactly three units");
    ok &= check(client.options().port == kFollowedPort,
                "truthy advertised port was not followed");
    ok &= check(client.options().protocolVersion == 622u,
                "decoder/network were not built after discovered version");

    {
        std::lock_guard<std::mutex> lock(logMutex);
        const std::string expected =
            "Connecting to 127.0.0.1:" + std::to_string(kFollowedPort) +
            " Factory Motd (Factory Level), version 1.20.40.9  (as 1.20.40)";
        ok &= check(connectionLog == expected, "conLog formatting/order mismatch");
    }
    responder.finish();
    ok &= check(responder.receivedPing() && responder.error().empty(),
                "local responder did not observe clean ping");
    return ok;
}

bool checkSlowConnectAllowedQueueGate() {
    PingResponder responder("1.20.40", 0, 35ms);
    auto options = baseOptions(responder.port());
    options.batchingInterval = 20;
    options.connectTimeout = 250;
    auto client = bedrock::createClient(options);
    std::atomic<int> allowed {0};
    std::atomic<bool> pumpPausedInsideSnapshot {false};
    std::atomic<bool> packetSurvivedSlowListener {false};
    client.onError([](const std::string&) {});
    client.onConnectAllowed([&]() {
        pumpPausedInsideSnapshot =
            !bedrock::BedrockNetworkClientTestAccess::queuePumpEnabled(
                client.network()
            );
        client.queue(
            "client_cache_status",
            bedrock::ProtoDefValue::object({
                {"enabled", bedrock::ProtoDefValue::boolean(false)}
            })
        );
        // More than three ordinary batching intervals: a native timer thread
        // would clear/drop this packet mid-listener without the EventEmitter
        // admission gate. Node cannot run its interval until emit returns.
        std::this_thread::sleep_for(70ms);
        packetSurvivedSlowListener =
            bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                client.network()
            ) == 1;
        ++allowed;
    });

    bool ok = check(waitFor([&]() { return allowed.load() == 1; }),
                    "slow connect_allowed listener did not run");
    ok &= check(
        pumpPausedInsideSnapshot.load() &&
            packetSurvivedSlowListener.load(),
        "queue pump ran during the connect_allowed EventEmitter snapshot"
    );
    client.close("slow-listener-cleanup");
    responder.finish();
    return ok;
}

bool runDiscoveryCase(
    const std::string& label,
    const std::string& requestedVersion,
    const std::string& advertisedVersion,
    std::optional<bool> followPort,
    uint16_t advertisedPort,
    const std::string& expectedVersion,
    bool expectFollow
) {
    PingResponder responder(advertisedVersion, advertisedPort);
    auto options = baseOptions(responder.port());
    if (!requestedVersion.empty()) {
        options.version = requestedVersion;
    }
    options.followPort = followPort;

    auto client = bedrock::createClient(options);
    std::atomic<int> allowed {0};
    client.onError([](const std::string&) {});
    client.onConnectAllowed([&]() { ++allowed; });
    bool ok = check(waitFor([&]() { return allowed.load() == 1; }),
                    label + ": connect_allowed missing");
    ok &= check(client.options().version == expectedVersion,
                label + ": selected version mismatch");
    const uint16_t expectedPort = expectFollow ? advertisedPort : responder.port();
    ok &= check(client.options().port == expectedPort,
                label + ": selected port mismatch");
    ok &= check(client.options().followPort ==
                    std::optional<bool>(followPort.value_or(true)),
                label + ": resolved followPort mismatch");
    responder.finish();
    ok &= check(responder.receivedPing() && responder.error().empty(),
                label + ": ping responder mismatch");
    return ok;
}

bool checkDiscoverySelection() {
    bool ok = true;
    ok &= runDiscoveryCase(
        "omitted/supported", "", "1.21.130", std::nullopt,
        23001, "1.21.130", true
    );
    ok &= runDiscoveryCase(
        "omitted/unknown", "", "9.8.7", false,
        23002, std::string(bedrock::CURRENT_VERSION), false
    );
    ok &= runDiscoveryCase(
        "explicit wins", "1.20.15", "1.21.130", false,
        23003, "1.20.15", false
    );
    ok &= runDiscoveryCase(
        "explicit current", std::string(bedrock::CURRENT_VERSION), "1.20.40", false,
        23004, std::string(bedrock::CURRENT_VERSION), false
    );
    ok &= runDiscoveryCase(
        "follow false", "", "1.20.40", false,
        23005, "1.20.40", false
    );
    ok &= runDiscoveryCase(
        "port zero", "", "1.20.40", true,
        0, "1.20.40", false
    );
    return ok;
}

bool checkCloseInsideConnectAllowed() {
    bool ok = true;
    for (int iteration = 0; iteration < 24; ++iteration) {
        PingResponder responder("1.20.40", 0, 0ms);
        auto options = baseOptions(responder.port());
        options.connectTimeout = 20;

        auto client = bedrock::createClient(options);
        std::atomic<int> allowed {0};
        std::atomic<int> lateErrors {0};
        std::atomic<int> lateStatuses {0};
        std::atomic<int> lateCloses {0};
        std::atomic<int64_t> closeMicros {-1};
        client.onError([&](const std::string&) { ++lateErrors; });
        client.onStatus([&](bedrock::ClientStatus) { ++lateStatuses; });
        client.onClose([&](const std::string&) { ++lateCloses; });
        client.onConnectAllowed([&]() {
            const auto begin = std::chrono::steady_clock::now();
            client.close("close-inside-connect-allowed");
            closeMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin
            ).count();
            ++allowed;
        });

        ok &= check(waitFor([&]() { return allowed.load() == 1; }),
                    "close-in-connect_allowed callback did not return");
        ok &= check(closeMicros.load() >= 0 && closeMicros.load() < 10'000,
                    "close-in-connect_allowed blocked on RakNet handshake");
        ok &= check(client.autoConnectStarted(),
                    "close race did not exercise synchronous auto-connect intent");
        ok &= check(!client.connectWorkerStarted(),
                    "blocking connect worker launched before EventEmitter snapshot ended");
        std::this_thread::sleep_for(50ms);
        ok &= check(client.status() == bedrock::ClientStatus::Disconnected,
                    "closed factory client resurrected after connect worker unwind");
        ok &= check(
            lateErrors.load() == 0 && lateStatuses.load() == 0 &&
                lateCloses.load() == 0,
            "close inside connect_allowed leaked a late network callback"
        );
        responder.finish();
        ok &= check(responder.error().empty(),
                    "close race ping responder failed");
    }


    // Close after the listener snapshot, once the tracked native worker has
    // started but is blocked in the RakNet handshake. requestStop must wake it
    // without racing fd destruction or leaking its eventual failure event.
    PingResponder activeResponder("1.20.40", 0, 20ms);
    auto activeOptions = baseOptions(activeResponder.port());
    activeOptions.connectTimeout = 500;
    auto active = bedrock::createClient(activeOptions);
    std::atomic<int> activeAllowed {0};
    std::atomic<int> activeErrors {0};
    std::atomic<int> activeStatuses {0};
    std::atomic<int> activeCloses {0};
    active.onError([&](const std::string&) { ++activeErrors; });
    active.onStatus([&](bedrock::ClientStatus) { ++activeStatuses; });
    active.onClose([&]() { ++activeCloses; });
    active.onConnectAllowed([&]() { ++activeAllowed; });
    ok &= check(waitFor([&]() { return active.connectWorkerStarted(); }),
                "active-close test did not launch connect worker");
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        active.network(),
        bedrock::ClientStatus::Connecting
    );
    activeStatuses = 0;
    const auto activeCloseBegin = std::chrono::steady_clock::now();
    active.close("active-handshake-close");
    const auto activeCloseElapsed =
        std::chrono::steady_clock::now() - activeCloseBegin;
    ok &= check(activeCloseElapsed < 25ms,
                "active-handshake close blocked on native timeout");
    ok &= check(waitFor([&]() {
                    return bedrock::ClientFactoryTestAccess::connectWorkerExited(
                        active
                    );
                }, 800ms),
                "active-handshake close did not finish deferred cleanup");
    ok &= check(active.status() == bedrock::ClientStatus::Disconnected,
                "active-handshake close resurrected network status");
    ok &= check(activeAllowed.load() == 1 && activeErrors.load() == 0 &&
                    activeStatuses.load() == 0 && activeCloses.load() == 1,
                "active-handshake close leaked a late callback");
    activeResponder.finish();

    // The close listener owns BedrockNetworkClient's close latch on this
    // thread. Destroying the last facade owner must not join a connect worker
    // which will itself close the network during finalization and wait on that
    // latch. A separate releaser lets the worker reach the exact WillClose
    // edge while the listener is still active.
    PingResponder destroyInCloseResponder("1.20.40", 0, 20ms);
    auto destroyInCloseOptions = baseOptions(destroyInCloseResponder.port());
    destroyInCloseOptions.connectTimeout = 35;
    std::unique_ptr<bedrock::Client> destroyInCloseOwner(
        new bedrock::Client(bedrock::createClient(destroyInCloseOptions))
    );
    std::shared_ptr<bedrock::BedrockNetworkClient> destroyInCloseNetwork;
    auto destroyInCloseState =
        bedrock::ClientFactoryTestAccess::weakState(*destroyInCloseOwner);
    std::atomic<bool> finalizationBarrierEntered {false};
    std::atomic<bool> releaseFinalizationBarrier {false};
    std::atomic<bool> destructiveCloseListenerEntered {false};
    std::atomic<bool> ownerResetReturned {false};
    bedrock::ClientFactoryTestAccess::setAfterNetworkConnectHook(
        *destroyInCloseOwner,
        [&]() {
            finalizationBarrierEntered = true;
            while (!releaseFinalizationBarrier.load()) {
                std::this_thread::yield();
            }
        }
    );
    destroyInCloseOwner->onError([](const std::string&) {});
    destroyInCloseOwner->onClose([&]() {
        destructiveCloseListenerEntered = true;
        destroyInCloseOwner.reset();
        ownerResetReturned = true;
    });
    destroyInCloseOwner->onConnectAllowed([]() {});
    const bool reachedFinalizationBarrier = waitFor(
        [&]() { return finalizationBarrierEntered.load(); },
        1200ms
    );
    destroyInCloseNetwork =
        bedrock::ClientFactoryTestAccess::sharedNetwork(*destroyInCloseOwner);
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        *destroyInCloseNetwork,
        bedrock::ClientStatus::Connecting
    );
    std::thread finalizationReleaser([&]() {
        (void)waitFor(
            [&]() { return destructiveCloseListenerEntered.load(); },
            800ms
        );
        releaseFinalizationBarrier = true;
    });
    const auto destructiveCloseBegin = std::chrono::steady_clock::now();
    destroyInCloseOwner->close("destroy-owner-inside-close");
    const auto destructiveCloseElapsed =
        std::chrono::steady_clock::now() - destructiveCloseBegin;
    finalizationReleaser.join();
    const bool destructiveWorkerExited = waitFor(
        [&]() { return destroyInCloseState.expired(); },
        1200ms
    );
    ok &= check(
        reachedFinalizationBarrier && destructiveCloseListenerEntered.load() &&
            ownerResetReturned.load() && !destroyInCloseOwner &&
            destructiveCloseElapsed < 500ms && destructiveWorkerExited &&
            destroyInCloseNetwork->status() ==
                bedrock::ClientStatus::Disconnected &&
            !bedrock::BedrockNetworkClientTestAccess::hasTransport(
                *destroyInCloseNetwork
            ) &&
            !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                *destroyInCloseNetwork
            ),
        "facade destruction inside close listener deadlocked worker finalization"
    );
    destroyInCloseResponder.finish();

    // A close-owner callback must also escape before joining preflight. The
    // preflight thread has already published the network here, but its
    // external connect_allowed listener deliberately starts a concurrent
    // network close and waits on the main thread's active close latch.
    PingResponder preflightCloseResponder("1.20.40", 0, 20ms);
    auto preflightCloseOptions = baseOptions(preflightCloseResponder.port());
    std::unique_ptr<bedrock::Client> preflightCloseOwner(
        new bedrock::Client(bedrock::createClient(preflightCloseOptions))
    );
    auto preflightCloseState =
        bedrock::ClientFactoryTestAccess::weakState(*preflightCloseOwner);
    std::mutex preflightNetworkMutex;
    std::shared_ptr<bedrock::BedrockNetworkClient> preflightNetwork;
    std::atomic<bool> preflightAllowedEntered {false};
    std::atomic<bool> allowPreflightClose {false};
    std::atomic<bool> preflightCloseAttempted {false};
    std::atomic<bool> preflightCloseReturned {false};
    std::atomic<bool> preflightOwnerResetReturned {false};
    preflightCloseOwner->onError([](const std::string&) {});
    preflightCloseOwner->onClose([&]() {
        allowPreflightClose = true;
        (void)waitFor(
            [&]() { return preflightCloseAttempted.load(); },
            500ms
        );
        preflightCloseOwner.reset();
        preflightOwnerResetReturned = true;
    });
    preflightCloseOwner->onConnectAllowed([&]() {
        preflightAllowedEntered = true;
        while (!allowPreflightClose.load()) std::this_thread::yield();
        std::shared_ptr<bedrock::BedrockNetworkClient> network;
        {
            std::lock_guard<std::mutex> lock(preflightNetworkMutex);
            network = preflightNetwork;
        }
        preflightCloseAttempted = true;
        network->close("preflight-concurrent-close");
        preflightCloseReturned = true;
    });
    const bool reachedPreflightListener = waitFor(
        [&]() { return preflightAllowedEntered.load(); },
        1200ms
    );
    {
        std::lock_guard<std::mutex> lock(preflightNetworkMutex);
        preflightNetwork =
            bedrock::ClientFactoryTestAccess::sharedNetwork(
                *preflightCloseOwner
            );
    }
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        *preflightNetwork,
        bedrock::ClientStatus::Connecting
    );
    const auto preflightOwnedCloseBegin = std::chrono::steady_clock::now();
    preflightCloseOwner->close("preflight-owner-close");
    const auto preflightOwnedCloseElapsed =
        std::chrono::steady_clock::now() - preflightOwnedCloseBegin;
    const bool preflightThreadExited = waitFor(
        [&]() {
            return preflightCloseReturned.load() &&
                preflightCloseState.expired();
        },
        1200ms
    );
    ok &= check(
        reachedPreflightListener && preflightCloseAttempted.load() &&
            preflightCloseReturned.load() &&
            preflightOwnerResetReturned.load() && !preflightCloseOwner &&
            preflightOwnedCloseElapsed < 500ms && preflightThreadExited &&
            preflightNetwork->status() ==
                bedrock::ClientStatus::Disconnected,
        "close-owner destruction joined preflight waiting on its latch"
    );
    preflightCloseResponder.finish();

    // The same facade-lifetime guarantee applies when the caller deliberately
    // enters through the exposed low-level network reference. emitClose's
    // lifetime lease must outlive its scope guard before State destroys the
    // network object.
    std::unique_ptr<bedrock::Client> networkCloseOwner(
        new bedrock::Client(baseOptions(9))
    );
    auto networkCloseState =
        bedrock::ClientFactoryTestAccess::weakState(*networkCloseOwner);
    auto* networkCloseRaw = &networkCloseOwner->network();
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        *networkCloseRaw,
        bedrock::ClientStatus::Connecting
    );
    std::atomic<int> networkCloseSnapshot {0};
    networkCloseOwner->onClose([&]() {
        ++networkCloseSnapshot;
        networkCloseOwner.reset();
    });
    networkCloseOwner->onClose([&]() { ++networkCloseSnapshot; });
    networkCloseRaw->close("network-reference-close");
    ok &= check(
        !networkCloseOwner && networkCloseSnapshot.load() == 2 &&
            waitFor([&]() { return networkCloseState.expired(); }, 500ms),
        "network().close facade destruction invalidated guard/lifetime order"
    );

    PingResponder nestedCloseResponder("1.20.40", 0, 20ms);
    auto nestedCloseOptions = baseOptions(nestedCloseResponder.port());
    nestedCloseOptions.connectTimeout = 500;
    auto nestedClose = bedrock::createClient(nestedCloseOptions);
    nestedClose.onError([](const std::string&) {});
    nestedClose.onConnectAllowed([]() {});
    std::atomic<bool> innerCloseCommitted {false};
    std::atomic<bool> insideInnerClose {false};
    nestedClose.onClose([&]() {
        if (!innerCloseCommitted.exchange(true)) {
            insideInnerClose = true;
            nestedClose.disconnect("inner-committed-disconnect");
            insideInnerClose = false;
        }
    });
    nestedClose.onClose([&]() {
        if (!insideInnerClose.load()) {
            throw std::runtime_error("outer close listener boom");
        }
    });
    ok &= check(waitFor([&]() { return nestedClose.connectWorkerStarted(); }),
                "nested-close test did not launch connect worker");
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        nestedClose.network(),
        bedrock::ClientStatus::Connecting
    );
    bool outerCloseThrowEscaped = false;
    try {
        nestedClose.close("outer-throwing-close");
    } catch (const std::exception& error) {
        outerCloseThrowEscaped = std::string(error.what()) ==
            "outer close listener boom";
    }
    ok &= check(
        outerCloseThrowEscaped && innerCloseCommitted.load() &&
            nestedClose.status() == bedrock::ClientStatus::Disconnected &&
            bedrock::ClientFactoryTestAccess::autoConnectCancelled(
                nestedClose
            ) &&
            bedrock::ClientFactoryTestAccess::deferredClose(nestedClose),
        "outer close rollback undid a committed recursive inner close"
    );
    ok &= check(
        waitFor([&]() {
            return bedrock::ClientFactoryTestAccess::connectWorkerExited(
                nestedClose
            );
        }, 800ms),
        "committed recursive inner close did not finish deferred worker"
    );
    nestedCloseResponder.finish();
    return ok;
}

bool checkThrowingConnectAllowed() {
    bool ok = true;
    {
        PingResponder responder("1.20.40", 0, 35ms);
        auto options = baseOptions(responder.port());
        auto client = bedrock::createClient(options);
        std::atomic<int> errors {0};
        std::atomic<int> laterListeners {0};
        std::string errorMessage;
        client.onError([&](const std::string& message) {
            errorMessage = message;
            ++errors;
        });
        client.onConnectAllowed([]() {
            throw std::runtime_error("connect_allowed boom");
        });
        client.onConnectAllowed([&]() { ++laterListeners; });

        ok &= check(waitFor([&]() { return errors.load() == 1; }),
                    "throwing connect_allowed did not reject init/emit error");
        ok &= check(errorMessage == "connect_allowed boom",
                    "throwing connect_allowed lost its exception message");
        ok &= check(client.autoConnectStarted() && client.connectWorkerStarted(),
                    "internal connect side effect was lost after later listener throw");
        ok &= check(laterListeners.load() == 0,
                    "EventEmitter continued after throwing connect_allowed listener");
        responder.finish();
    }

    {
        PingResponder responder("1.20.40", 0, 35ms);
        auto options = baseOptions(responder.port());
        auto client = bedrock::createClient(options);
        std::atomic<int> staleErrors {0};
        client.onError([&](const std::string&) { ++staleErrors; });
        client.onConnectAllowed([&]() {
            client.close("close-before-throw");
            throw std::runtime_error("closed connect_allowed boom");
        });

        ok &= check(waitFor([&]() { return client.initialized(); }),
                    "close+throw connect_allowed never reached initialized state");
        std::this_thread::sleep_for(80ms);
        ok &= check(staleErrors.load() == 0,
                    "close+throw reused an error listener removed by close()");
        ok &= check(!client.connectWorkerStarted(),
                    "close+throw started a cancelled blocking connect worker");
        responder.finish();
    }

    {
        PingResponder responder("1.20.40", 0, 35ms);
        auto options = baseOptions(responder.port());
        std::atomic<int> sinkCalls {0};
        std::string sinkMessage;
        options.conLog = [](const std::string&) { throw 7; };
        options.onUnhandledAsyncError = [&](const std::string& message) {
            sinkMessage = message;
            ++sinkCalls;
        };
        auto client = bedrock::createClient(options);
        ok &= check(
            waitFor([&]() { return sinkCalls.load() == 1; }) &&
                sinkMessage == "Unknown asynchronous error" &&
                !client.initialized(),
            "non-std preflight callback throw escaped its worker boundary"
        );
        std::this_thread::sleep_for(30ms);
        ok &= check(sinkCalls.load() == 1,
                    "non-std preflight throw reached async sink more than once");
        responder.finish();
    }

    {
        PingResponder responder("1.20.40", 0, 35ms);
        auto options = baseOptions(responder.port());
        std::atomic<int> sinkCalls {0};
        std::string sinkMessage;
        options.onUnhandledAsyncError = [&](const std::string& message) {
            sinkMessage = message;
            ++sinkCalls;
        };
        auto client = bedrock::createClient(options);
        std::atomic<int> allowed {0};
        client.onError([](const std::string&) { throw 9; });
        client.onConnectAllowed([&]() { ++allowed; });
        ok &= check(
            waitFor([&]() {
                return allowed.load() == 1 && sinkCalls.load() == 1;
            }) && sinkMessage == "Unknown asynchronous error",
            "non-std network error listener throw escaped connect worker"
        );
        std::this_thread::sleep_for(30ms);
        ok &= check(sinkCalls.load() == 1,
                    "non-std worker throw reached async sink more than once");
        responder.finish();
    }
    return ok;
}

bool checkPrototypeVersionAdvertisement() {
    bool ok = true;
    PingResponder responder("constructor", 0, 0ms);
    auto options = baseOptions(responder.port());

    auto client = bedrock::createClient(options);
    std::atomic<int> errors {0};
    std::atomic<int> allowed {0};
    std::string errorMessage;
    client.onError([&](const std::string& message) {
        errorMessage = message;
        ++errors;
    });
    client.onConnectAllowed([&]() { ++allowed; });
    ok &= check(waitFor([&]() { return errors.load() == 1; }),
                "prototype-key advertisement did not emit init error");
    ok &= check(
        errorMessage == "Cannot read properties of undefined (reading 'types')",
        "prototype-key advertisement emitted wrong TypeError message"
    );
    ok &= check(client.options().version == "constructor",
                "inherited truthy Versions key incorrectly fell back to CURRENT_VERSION");
    ok &= check(!client.initialized() && !client.autoConnectStarted(),
                "prototype-key init failure constructed/launched network");
    ok &= check(allowed.load() == 0,
                "prototype-key init failure emitted connect_allowed");
    responder.finish();

    bool directExact = false;
    try {
        auto directOptions = baseOptions(19132);
        directOptions.version = "constructor";
        bedrock::Client direct(directOptions);
    } catch (const std::exception& e) {
        directExact = std::string(e.what()) ==
            "Cannot read properties of undefined (reading 'types')";
    }
    ok &= check(directExact, "direct prototype-key Client error mismatch");
    return ok;
}

bool checkExplicitEmptyVersionAdvertisement() {
    PingResponder responder("1.20.40", 0, 35ms);
    auto options = baseOptions(responder.port());
    options.version = "";
    std::atomic<int> thrownErrorSinkCalls {0};
    std::string thrownErrorSinkMessage;
    options.onUnhandledAsyncError = [&](const std::string& message) {
        thrownErrorSinkMessage = message;
        ++thrownErrorSinkCalls;
    };
    auto client = bedrock::createClient(options);
    bool ok = check(client.options().version.empty(),
                    "explicit empty version was defaulted before discovery");
    std::atomic<int> errors {0};
    std::atomic<int> allowed {0};
    std::string errorMessage;
    client.onError([&](const std::string& message) {
        errorMessage = message;
        ++errors;
        throw std::runtime_error("error listener boom");
    });
    client.onConnectAllowed([&]() { ++allowed; });
    ok &= check(waitFor([&]() { return errors.load() == 1; }),
                "normal factory explicit empty version did not emit error");
    ok &= check(errorMessage == "Unsupported version ",
                "normal factory explicit empty version error was not exact");
    ok &= check(
        waitFor([&]() { return thrownErrorSinkCalls.load() == 1; }) &&
            thrownErrorSinkMessage == "error listener boom" &&
            client.takeUnhandledAsyncError() ==
                std::optional<std::string>("error listener boom"),
        "throwing onError listener did not reach unhandled async boundary"
    );
    ok &= check(allowed.load() == 0 && !client.initialized() &&
                    client.options().version.empty(),
                "explicit empty version incorrectly used ping discovery");
    responder.finish();
    return ok;
}

bool checkPingErrorAndCloseBeforePong() {
    bool ok = true;
    // Hold a real local UDP endpoint but reply after the pinger's fixed
    // deadline, avoiding any dependency on whether an arbitrary port is free.
    PingResponder timeoutResponder("1.20.40", 0, 1200ms);
    auto errorOptions = baseOptions(timeoutResponder.port());
    std::atomic<int> errors {0};
    std::atomic<int> suppressedUnhandledSink {0};
    errorOptions.onUnhandledAsyncError = [&](const std::string&) {
        ++suppressedUnhandledSink;
    };
    std::string errorMessage;
    const auto pingBegin = std::chrono::steady_clock::now();
    auto failed = bedrock::createClient(errorOptions);
    failed.onError([&](const std::string& message) {
        errorMessage = message;
        ++errors;
    });
    ok &= check(waitFor([&]() { return errors.load() == 1; }, 1400ms),
                "ping timeout error was not emitted");
    ok &= check(errorMessage == "Ping timed out", "ping timeout message mismatch");
    const auto pingElapsed = std::chrono::steady_clock::now() - pingBegin;
    ok &= check(pingElapsed >= 850ms && pingElapsed < 1400ms,
                "factory ping did not use native fixed ~1000ms timeout");
    ok &= check(!failed.initialized(), "ping failure initialized client");
    ok &= check(!failed.autoConnectStarted(), "ping failure launched connect");
    ok &= check(suppressedUnhandledSink.load() == 0 &&
                    !failed.takeUnhandledAsyncError().has_value(),
                "ordinary onError did not suppress unhandled async sink");
    timeoutResponder.finish();
    ok &= check(timeoutResponder.receivedPing() && timeoutResponder.error().empty(),
                "timeout responder did not receive the factory ping");

    PingResponder unhandledResponder("1.20.40", 0, 1200ms);
    auto unhandledOptions = baseOptions(unhandledResponder.port());
    std::atomic<int> unhandledSinkCalls {0};
    std::string unhandledSinkMessage;
    unhandledOptions.onUnhandledAsyncError = [&](const std::string& message) {
        unhandledSinkMessage = message;
        ++unhandledSinkCalls;
    };
    auto unhandled = bedrock::createClient(unhandledOptions);
    ok &= check(
        waitFor([&]() { return unhandledSinkCalls.load() == 1; }, 1400ms),
        "unhandled ping timeout did not reach the preinstalled sink"
    );
    const auto storedUnhandled = unhandled.takeUnhandledAsyncError();
    ok &= check(
        unhandledSinkCalls.load() == 1 &&
            unhandledSinkMessage == "Ping timed out" &&
            storedUnhandled == std::optional<std::string>("Ping timed out") &&
            !unhandled.takeUnhandledAsyncError().has_value(),
        "unhandled async sink/storage did not preserve exactly-once timeout"
    );
    unhandledResponder.finish();

    PingResponder preparationFailureResponder("1.20.40", 0, 35ms);
    auto preparationFailureOptions = baseOptions(
        preparationFailureResponder.port()
    );
    preparationFailureOptions.loginPacket = {0x01};
    preparationFailureOptions.authCacheRoot =
        std::filesystem::current_path() / "CMakeLists.txt";
    std::atomic<int> preparationFailureSinkCalls {0};
    std::string preparationFailureSinkMessage;
    preparationFailureOptions.onUnhandledAsyncError =
        [&](const std::string& message) {
            preparationFailureSinkMessage = message;
            ++preparationFailureSinkCalls;
        };
    auto preparationFailure = bedrock::createClient(
        preparationFailureOptions
    );
    std::atomic<int> preparationAllowed {0};
    preparationFailure.onConnectAllowed([&]() { ++preparationAllowed; });
    ok &= check(
        waitFor([&]() {
            return preparationAllowed.load() == 1 &&
                preparationFailureSinkCalls.load() == 1;
        }),
        "worker-side login preparation failure did not reach async sink"
    );
    std::this_thread::sleep_for(80ms);
    const auto storedPreparationFailure =
        preparationFailure.takeUnhandledAsyncError();
    ok &= check(
        preparationFailure.connectWorkerStarted() &&
            preparationFailureSinkCalls.load() == 1 &&
            !preparationFailureSinkMessage.empty() &&
            storedPreparationFailure ==
                std::optional<std::string>(preparationFailureSinkMessage),
        "login preparation failure was recorded/emitted more than once"
    );
    preparationFailureResponder.finish();

    auto serializedSinkOptions = baseOptions(9);
    std::atomic<int> sinkActive {0};
    std::atomic<int> sinkMaxActive {0};
    std::atomic<int> serializedSinkCalls {0};
    std::mutex serializedMessagesMutex;
    std::vector<std::string> serializedMessages;
    serializedSinkOptions.onUnhandledAsyncError =
        [&](const std::string& message) {
            const int active = sinkActive.fetch_add(1) + 1;
            int observed = sinkMaxActive.load();
            while (observed < active &&
                   !sinkMaxActive.compare_exchange_weak(observed, active)) {}
            std::this_thread::sleep_for(40ms);
            {
                std::lock_guard<std::mutex> lock(serializedMessagesMutex);
                serializedMessages.push_back(message);
            }
            ++serializedSinkCalls;
            --sinkActive;
        };
    bedrock::Client serializedSinkClient(serializedSinkOptions);
    std::atomic<bool> releaseSinkRecords {false};
    auto recordSink = [&](const std::string& message) {
        while (!releaseSinkRecords.load()) std::this_thread::yield();
        bedrock::ClientFactoryTestAccess::recordUnhandledAsyncError(
            serializedSinkClient,
            message
        );
    };
    std::thread sinkA(recordSink, "preflight-boundary");
    std::thread sinkB(recordSink, "connect-boundary");
    releaseSinkRecords = true;
    sinkA.join();
    sinkB.join();
    const auto serializedStored =
        serializedSinkClient.takeUnhandledAsyncError();
    ok &= check(
        serializedSinkCalls.load() == 2 && sinkMaxActive.load() == 1 &&
            serializedMessages.size() == 2 && serializedStored.has_value() &&
            (*serializedStored == "preflight-boundary" ||
             *serializedStored == "connect-boundary"),
        "preflight/connect unhandled sinks ran concurrently or lost an occurrence"
    );

    PingResponder delayed("1.20.40", 0, 80ms);
    auto closeOptions = baseOptions(delayed.port());
    auto closed = bedrock::createClient(closeOptions);
    std::atomic<int> allowed {0};
    closed.onError([](const std::string&) {});
    closed.onConnectAllowed([&]() { ++allowed; });
    closed.close();
    ok &= check(waitFor([&]() { return closed.initialized(); }),
                "close-before-pong prevented delayed init");
    std::this_thread::sleep_for(20ms);
    ok &= check(allowed.load() == 0,
                "close-before-pong retained connect_allowed listeners");
    ok &= check(!closed.autoConnectStarted(),
                "close-before-pong retained internal auto-connect listener");
    delayed.finish();

    bool realmsExact = false;
    try {
        auto realms = baseOptions(19132);
        realms.realms = true;
        auto unsupported = bedrock::createClient(realms);
        (void) unsupported;
    } catch (const std::exception& e) {
        realmsExact = std::string(e.what()) == "Realms are not supported";
    }
    ok &= check(realmsExact, "Realms slice was not explicitly rejected");

    // A Node error listener may synchronously dispose its Client. Exercise
    // the equivalent C++ path on the tracked connect worker: shutdown must
    // neither join itself nor close RakNet while connect() is still unwinding.
    PingResponder selfDestroyResponder("1.20.40", 0);
    auto selfDestroyOptions = baseOptions(selfDestroyResponder.port());
    std::unique_ptr<bedrock::Client> owner = std::make_unique<bedrock::Client>(
        bedrock::createClient(selfDestroyOptions)
    );
    std::atomic<bool> destroyedOnWorker {false};
    std::atomic<int> remainingSnapshotListeners {0};
    owner->onError([&](const std::string&) {
        owner.reset();
        destroyedOnWorker = true;
    });
    owner->onError([&](const std::string&) {
        ++remainingSnapshotListeners;
    });
    ok &= check(waitFor([&]() { return destroyedOnWorker.load(); }),
                "Client destruction from connect-worker error callback hung");
    ok &= check(waitFor([&]() { return remainingSnapshotListeners.load() == 1; }),
                "Client destruction skipped remaining error snapshot listener");
    selfDestroyResponder.finish();
    return ok;
}

bool checkInvalidAuthenticationBoundary() {
    bool ok = true;
    const std::string authError =
        "Missing 'flow' argument in options. See docs for more information.";

    const auto makeInvalidOptions = [&]() {
        auto options = baseOptions(9);
        options.delayedInit = true;
        options.offline = false;
        options.authTitle = std::string(bedrock::Titles::MinecraftIOS);
        options.deviceType = "ExplicitDevice";
        options.flow = "";
        options.profilesFolder = true;
        return options;
    };

    const auto checkUnhandledCase = [&](bool throwingErrorListener) {
        auto options = makeInvalidOptions();
        std::atomic<bool> connectReturned {false};
        std::atomic<bool> sinkBeforeConnectReturn {false};
        std::atomic<bool> queueRunningInSink {false};
        std::atomic<bool> transportPresentInSink {true};
        std::atomic<bool> connectWorkerPresentInSink {true};
        std::atomic<int> sinkCalls {0};
        std::atomic<int> errorCalls {0};
        std::atomic<bool> queueRunningInError {true};
        std::mutex messageMutex;
        std::string sinkMessage;
        std::string listenerMessage;
        bedrock::Client* clientPointer = nullptr;

        options.onUnhandledAsyncError = [&](const std::string& message) {
            if (!connectReturned.load()) sinkBeforeConnectReturn = true;
            if (clientPointer) {
                queueRunningInSink =
                    bedrock::BedrockNetworkClientTestAccess::queueRunning(
                        clientPointer->network()
                    );
                transportPresentInSink =
                    bedrock::BedrockNetworkClientTestAccess::hasTransport(
                        clientPointer->network()
                    );
                connectWorkerPresentInSink =
                    clientPointer->connectWorkerStarted();
            }
            {
                std::lock_guard<std::mutex> lock(messageMutex);
                sinkMessage = message;
            }
            ++sinkCalls;
        };

        bedrock::Client client(std::move(options));
        clientPointer = &client;
        client.init();
        if (throwingErrorListener) {
            client.onError([&](const std::string& message) {
                {
                    std::lock_guard<std::mutex> lock(messageMutex);
                    listenerMessage = message;
                }
                queueRunningInError =
                    bedrock::BedrockNetworkClientTestAccess::queueRunning(
                        client.network()
                    );
                ++errorCalls;
                throw std::runtime_error("public auth error listener boom");
            });
        }

        bool connectResult = true;
        bool connectThrew = false;
        try {
            connectResult = client.connect();
        } catch (...) {
            connectThrew = true;
        }
        connectReturned = true;

        const bool queueRunningAfterConnect =
            bedrock::BedrockNetworkClientTestAccess::queueRunning(
                client.network()
            );
        const bool transportPresentAfterConnect =
            bedrock::BedrockNetworkClientTestAccess::hasTransport(
                client.network()
            );
        const bool connectWorkerPresentAfterConnect =
            client.connectWorkerStarted();
        const bool delivered = waitFor(
            [&]() { return sinkCalls.load() == 1; },
            500ms
        );
        const auto stored = client.takeUnhandledAsyncError();

        std::string observedSinkMessage;
        std::string observedListenerMessage;
        {
            std::lock_guard<std::mutex> lock(messageMutex);
            observedSinkMessage = sinkMessage;
            observedListenerMessage = listenerMessage;
        }
        const std::string expectedUnhandled = throwingErrorListener
            ? "public auth error listener boom"
            : authError;
        const std::string label = throwingErrorListener
            ? "throwing auth error listener"
            : "missing auth error listener";

        ok &= check(
            !connectThrew && !connectResult && queueRunningAfterConnect &&
                !transportPresentAfterConnect &&
                !connectWorkerPresentAfterConnect,
            label +
                " escaped connect or started transport instead of only queue"
        );
        ok &= check(
            delivered && !sinkBeforeConnectReturn.load() &&
                queueRunningInSink.load() &&
                !transportPresentInSink.load() &&
                !connectWorkerPresentInSink.load(),
            label +
                " did not reach async boundary after connect/queue publication"
        );
        ok &= check(
            observedSinkMessage == expectedUnhandled &&
                stored == std::optional<std::string>(expectedUnhandled),
            label + " did not preserve the exact ignored-Promise rejection"
        );
        if (throwingErrorListener) {
            ok &= check(
                errorCalls.load() == 1 &&
                    observedListenerMessage == authError &&
                    !queueRunningInError.load(),
                "throwing auth error listener did not observe the exact "
                "pre-startQueue error"
            );
        } else {
            ok &= check(
                errorCalls.load() == 0,
                "missing-listener auth boundary unexpectedly invoked a listener"
            );
        }
        std::this_thread::sleep_for(20ms);
        ok &= check(
            sinkCalls.load() == 1 &&
                !client.takeUnhandledAsyncError().has_value(),
            label + " was reported or stored more than once"
        );
        client.close();
        ok &= check(
            !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                client.network()
            ),
            label + " close did not stop the auth-only queue"
        );
    };

    checkUnhandledCase(false);
    checkUnhandledCase(true);

    auto closeOptions = makeInvalidOptions();
    std::atomic<int> unexpectedUnhandled {0};
    closeOptions.onUnhandledAsyncError = [&](const std::string&) {
        ++unexpectedUnhandled;
    };
    bedrock::Client closeInError(std::move(closeOptions));
    closeInError.init();
    std::vector<int> order;
    std::string firstMessage;
    std::string secondMessage;
    bool queueRunningAfterFirstClose = true;
    closeInError.onError([&](const std::string& message) {
        order.push_back(1);
        firstMessage = message;
        closeInError.close("close-inside-auth-error");
        queueRunningAfterFirstClose =
            bedrock::BedrockNetworkClientTestAccess::queueRunning(
                closeInError.network()
            );
    });
    closeInError.onError([&](const std::string& message) {
        order.push_back(2);
        secondMessage = message;
    });
    bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
        closeInError.network(),
        [&]() { order.push_back(3); }
    );

    bool closeConnectResult = true;
    bool closeConnectThrew = false;
    try {
        closeConnectResult = closeInError.connect();
    } catch (...) {
        closeConnectThrew = true;
    }
    const bool queueRestartedAfterErrorSnapshot =
        bedrock::BedrockNetworkClientTestAccess::queueRunning(
            closeInError.network()
        );
    ok &= check(
        !closeConnectThrew && !closeConnectResult &&
            order == std::vector<int>({1, 2, 3}) &&
            firstMessage == authError && secondMessage == authError,
        "close inside first auth error listener invalidated the listener "
        "snapshot or startQueue order"
    );
    ok &= check(
        !queueRunningAfterFirstClose && queueRestartedAfterErrorSnapshot &&
            closeInError.status() == bedrock::ClientStatus::Disconnected &&
            !bedrock::BedrockNetworkClientTestAccess::hasTransport(
                closeInError.network()
            ) && !closeInError.connectWorkerStarted(),
        "close inside auth error prevented mandatory post-emit startQueue"
    );
    closeInError.close("second-close-after-auth-error");
    std::this_thread::sleep_for(20ms);
    ok &= check(
        !bedrock::BedrockNetworkClientTestAccess::queueRunning(
            closeInError.network()
        ) && unexpectedUnhandled.load() == 0 &&
            !closeInError.takeUnhandledAsyncError().has_value(),
        "second close did not clear the mandatory auth-only queue"
    );

    return ok;
}

bool checkSuppliedAuthflowBoundary() {
    bool ok = true;

    const auto looksLikeBase64Spki = [](const std::string& value) {
        if (value.size() < 100) return false;
        for (const unsigned char character : value) {
            const bool base64 =
                (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') ||
                character == '+' || character == '/' || character == '=';
            if (!base64) return false;
        }
        return true;
    };

    const auto suppliedOptions = [](
        uint16_t port,
        const std::shared_ptr<bedrock::Authflow>& authflow,
        bool delayedInit
    ) {
        auto options = baseOptions(port);
        options.delayedInit = delayedInit;
        options.offline = false;
        // Both are explicitly present/falsy in JS. A supplied truthy
        // options.authflow bypasses PrismarineAuth's constructor validation.
        options.authTitle = std::string("");
        options.flow = "";
        options.deviceType = "ExplicitDevice";
        // fs.existsSync(true) would enter PrismarineAuth's caught fallback.
        // The supplied object must bypass that cache path completely while
        // leaving the public property Boolean(true).
        options.profilesFolder = true;
        options.authflow = authflow;
        return options;
    };

    {
        std::atomic<int> invocations {0};
        std::string publicKey;
        const auto supplied = std::make_shared<bedrock::Authflow>(
            [&](std::string value) -> bedrock::MinecraftBedrockTokenFuture {
                publicKey = std::move(value);
                ++invocations;
                throw std::runtime_error("supplied authflow sync boom");
            }
        );
        auto options = suppliedOptions(9, supplied, true);
        // A sync custom-method throw is outside auth.js's Promise.catch, so a
        // truthy password must not convert it into the password-warning path.
        options.password = "present-but-not-a-promise-rejection";
        bedrock::Client client(std::move(options));
        client.init();

        std::vector<std::string> order;
        std::string errorMessage;
        bool queueRunningInsideError = true;
        bool optionsVisibleInsideError = false;
        bool connectReturnedInsideError = true;
        bool connectReturned = false;
        client.onError([&](const std::string& message) {
            order.push_back("error");
            errorMessage = message;
            queueRunningInsideError =
                bedrock::BedrockNetworkClientTestAccess::queueRunning(
                    client.network()
                );
            const auto visible = client.optionsSnapshot();
            optionsVisibleInsideError =
                visible.authTitle == std::optional<std::string>("") &&
                visible.flow.empty() && visible.deviceType == "ExplicitDevice" &&
                visible.profilesFolder.isBoolean() &&
                visible.profilesFolder.booleanValue() &&
                visible.authflow == supplied;
            connectReturnedInsideError = connectReturned;
        });
        bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
            client.network(),
            [&]() { order.push_back("before-queue"); }
        );

        bool connectResult = true;
        bool connectThrew = false;
        try {
            connectResult = client.connect();
        } catch (...) {
            connectThrew = true;
        }
        connectReturned = true;
        order.push_back("returned");

        const auto networkOptions = client.network().options();
        ok &= check(
            !connectThrew && !connectResult &&
                order == std::vector<std::string>({
                    "error", "before-queue", "returned"
                }) && errorMessage == "supplied authflow sync boom" &&
                !queueRunningInsideError && !connectReturnedInsideError,
            "supplied authflow sync throw did not precede startQueue/return"
        );
        ok &= check(
            invocations.load() == 1 && looksLikeBase64Spki(publicKey) &&
                optionsVisibleInsideError &&
                client.options().authflow == supplied &&
                networkOptions.authflow == supplied,
            "supplied authflow lost invocation, x509 argument, or identity"
        );
        ok &= check(
            bedrock::BedrockNetworkClientTestAccess::queueRunning(
                client.network()
            ) &&
                !bedrock::BedrockNetworkClientTestAccess::hasTransport(
                    client.network()
                ) &&
                bedrock::BedrockNetworkClientTestAccess::
                    effectiveAuthenticationCacheRoot(client.network()).empty(),
            "supplied authflow sync throw constructed cache/transport or lost queue"
        );
        client.close();
    }

    const auto checkRejectedFuture = [&](bool closeInsideError) {
        PingResponder responder("1.20.40", 0, 35ms);
        auto promise = std::make_shared<
            std::promise<bedrock::MinecraftBedrockTokenChains>
        >();
        std::atomic<int> invocations {0};
        std::string publicKey;
        const auto supplied = std::make_shared<bedrock::Authflow>(
            [&](std::string value) -> bedrock::MinecraftBedrockTokenFuture {
                auto future = promise->get_future();
                publicKey = std::move(value);
                ++invocations;
                return future;
            }
        );
        auto options = suppliedOptions(responder.port(), supplied, false);
        std::atomic<bool> createClientReturned {false};
        auto client = bedrock::createClient(std::move(options));
        createClientReturned = true;

        std::atomic<int> errors {0};
        std::atomic<bool> returnedBeforeError {false};
        std::atomic<bool> queueRunningInsideError {false};
        std::atomic<bool> queuePumpInsideError {false};
        std::atomic<bool> transportInsideError {true};
        std::atomic<bool> queueStoppedByErrorClose {false};
        std::string errorMessage;
        client.onError([&](const std::string& message) {
            errorMessage = message;
            returnedBeforeError = createClientReturned.load();
            queueRunningInsideError =
                bedrock::BedrockNetworkClientTestAccess::queueRunning(
                    client.network()
                );
            queuePumpInsideError =
                bedrock::BedrockNetworkClientTestAccess::queuePumpEnabled(
                    client.network()
                );
            transportInsideError =
                bedrock::BedrockNetworkClientTestAccess::hasTransport(
                    client.network()
                );
            if (closeInsideError) {
                client.close("close-inside-supplied-authflow-rejection");
                queueStoppedByErrorClose =
                    !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                        client.network()
                    );
            }
            ++errors;
        });

        const bool reachedAuthflow = waitFor(
            [&]() { return invocations.load() == 1; },
            800ms
        );
        const bool queueAndWorkerStarted = reachedAuthflow && waitFor(
            [&]() {
                return client.connectWorkerStarted() &&
                    bedrock::BedrockNetworkClientTestAccess::queueRunning(
                        client.network()
                    ) &&
                    bedrock::BedrockNetworkClientTestAccess::queuePumpEnabled(
                        client.network()
                    );
            },
            800ms
        );
        promise->set_exception(std::make_exception_ptr(
            std::runtime_error("supplied authflow future rejected")
        ));

        const bool delivered = waitFor(
            [&]() { return errors.load() == 1; },
            800ms
        );
        const bool workerExited = waitFor(
            [&]() {
                return bedrock::ClientFactoryTestAccess::connectWorkerExited(
                    client
                );
            },
            800ms
        );
        const bool queueRunningAfterHandledError =
            bedrock::BedrockNetworkClientTestAccess::queueRunning(
                client.network()
            );
        const auto visible = client.optionsSnapshot();

        const std::string label = closeInsideError
            ? "close-inside supplied authflow rejection"
            : "handled supplied authflow rejection";
        ok &= check(
            reachedAuthflow && queueAndWorkerStarted &&
                delivered && workerExited && returnedBeforeError.load() &&
                errorMessage == "supplied authflow future rejected" &&
                queueRunningInsideError.load() &&
                queuePumpInsideError.load() &&
                !transportInsideError.load(),
            label + " did not settle after the public queue/return boundary"
        );
        ok &= check(
            invocations.load() == 1 && looksLikeBase64Spki(publicKey) &&
                visible.authflow == supplied &&
                visible.authTitle == std::optional<std::string>("") &&
                visible.flow.empty() && visible.profilesFolder.isBoolean() &&
                visible.profilesFolder.booleanValue() &&
                client.network().options().authflow == supplied &&
                !bedrock::BedrockNetworkClientTestAccess::hasTransport(
                    client.network()
                ) && !client.takeUnhandledAsyncError().has_value(),
            label + " lost public identity/options or created transport"
        );
        if (closeInsideError) {
            ok &= check(
                queueStoppedByErrorClose.load() &&
                    !queueRunningAfterHandledError,
                "close inside supplied authflow rejection resurrected queue"
            );
        } else {
            ok &= check(
                queueRunningAfterHandledError,
                "handled supplied authflow rejection stopped startQueue"
            );
            client.close("close-after-handled-supplied-authflow-rejection");
        }
        responder.finish();
    };

    checkRejectedFuture(false);
    checkRejectedFuture(true);

    {
        // The second chain is the only JWT auth.js decodes for the Xbox
        // profile. Signature verification is intentionally absent there.
        const std::string profileJwt =
            "e30."
            "eyJleHRyYURhdGEiOnsiZGlzcGxheU5hbWUiOiJJbmplY3RlZCIs"
            "ImlkZW50aXR5IjoidXVpZCIsIlhVSUQiOiI0MiJ9fQ."
            "c2ln";
        std::atomic<int> invocations {0};
        const auto supplied = std::make_shared<bedrock::Authflow>(
            [&](std::string) {
                ++invocations;
                return bedrock::makeReadyAuthflowFuture(
                    bedrock::MinecraftBedrockTokenChains {
                        "eyJhbGciOiJFUzM4NCIsIng1dSI6ImR1bW15LXJvb3Qta2V5In0."
                        "eyJjZXJ0aWZpY2F0ZUF1dGhvcml0eSI6dHJ1ZX0.c2ln",
                        profileJwt
                    }
                );
            }
        );
        auto options = suppliedOptions(9, supplied, true);
        bedrock::Client client(std::move(options));
        client.init();
        std::atomic<int> errors {0};
        client.onError([&](const std::string&) { ++errors; });
        bool reachedPreTransport = false;
        bool loginPacketBuilt = false;
        bedrock::BedrockNetworkClientTestAccess::setBeforeTransportInstallHook(
            client.network(),
            [&]() {
                reachedPreTransport = true;
                loginPacketBuilt =
                    !client.network().options().loginPacket.empty();
                client.close("resolved-authflow-before-transport-install");
            }
        );

        const bool connectResult = client.connect();
        ok &= check(
            !connectResult && invocations.load() == 1 &&
                reachedPreTransport && loginPacketBuilt &&
                errors.load() == 0 &&
                client.options().authflow == supplied &&
                !bedrock::BedrockNetworkClientTestAccess::hasTransport(
                    client.network()
                ) &&
                !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                    client.network()
                ),
            "resolved supplied authflow did not build chains before transport"
        );
    }

    return ok;
}

bool checkEventLifetimeAndRakNetBoundaries() {
    bool ok = true;

    bedrock::BedrockNetworkClient connectDuringClose({
        .host = "127.0.0.1",
        .port = 9,
        .username = "ConnectDuringClose",
        .profile = "ConnectDuringClose",
        .version = "1.20.40",
        .offline = true,
        .connectTimeoutMs = 35,
        .batchingIntervalMs = 30,
        .trackWorld = false
    });
    connectDuringClose.onError([](const std::string&) {});
    ok &= check(
        connectDuringClose.prepareConnectLifecycle(true),
        "connect-during-close fixture did not prepare paused queue"
    );
    connectDuringClose.queue(
        "client_cache_status",
        bedrock::ProtoDefValue::object({
            {"enabled", bedrock::ProtoDefValue::boolean(false)}
        })
    );
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        connectDuringClose,
        bedrock::ClientStatus::Connecting
    );
    std::atomic<bool> closeListenerEntered {false};
    std::atomic<bool> releaseCloseListener {false};
    std::atomic<bool> closeThrowEscaped {false};
    std::atomic<bool> transportInstallEntered {false};
    std::atomic<bool> releaseTransportInstall {false};
    std::atomic<bool> concurrentConnectReturned {false};
    bedrock::BedrockNetworkClientTestAccess::setBeforeTransportInstallHook(
        connectDuringClose,
        [&]() {
            transportInstallEntered = true;
            while (!releaseTransportInstall.load()) std::this_thread::yield();
        }
    );
    connectDuringClose.onClose([&]() {
        closeListenerEntered = true;
        while (!releaseCloseListener.load()) std::this_thread::yield();
        throw std::runtime_error("connect-during-close rollback");
    });
    std::thread closeWithBarrier([&]() {
        try {
            connectDuringClose.close("throwing-close-barrier");
        } catch (const std::exception& error) {
            closeThrowEscaped = std::string(error.what()) ==
                "connect-during-close rollback";
        }
    });
    const bool reachedCloseListener = waitFor(
        [&]() { return closeListenerEntered.load(); }
    );
    std::thread concurrentConnector([&]() {
        (void)connectDuringClose.connect();
        concurrentConnectReturned = true;
    });
    const bool reachedTransportInstall = waitFor(
        [&]() { return transportInstallEntered.load(); }
    );
    std::this_thread::sleep_for(120ms);
    const bool stayedPausedInsideClose =
        bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
            connectDuringClose
        ) == 1 &&
        !bedrock::BedrockNetworkClientTestAccess::queuePumpEnabled(
            connectDuringClose
        );
    releaseCloseListener = true;
    closeWithBarrier.join();
    const bool resumedAfterRollback = waitFor(
        [&]() {
            return bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                connectDuringClose
            ) == 0;
        },
        500ms
    );
    releaseTransportInstall = true;
    concurrentConnector.join();
    ok &= check(
        reachedCloseListener && reachedTransportInstall &&
            stayedPausedInsideClose && closeThrowEscaped.load() &&
            resumedAfterRollback && concurrentConnectReturned.load(),
        "concurrent connect enabled queue inside throwing close snapshot"
    );

    bedrock::BedrockNetworkClient startDuringClose({
        .host = "127.0.0.1",
        .port = 9,
        .username = "StartDuringClose",
        .profile = "StartDuringClose",
        .version = "1.20.40",
        .offline = true,
        .connectTimeoutMs = 35,
        .batchingIntervalMs = 30,
        .trackWorld = false
    });
    startDuringClose.onError([](const std::string&) {});
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        startDuringClose,
        bedrock::ClientStatus::Connecting
    );
    std::atomic<bool> startCloseEntered {false};
    std::atomic<bool> releaseStartClose {false};
    std::atomic<bool> startCloseEscaped {false};
    std::atomic<bool> startInstallEntered {false};
    std::atomic<bool> releaseStartInstall {false};
    bedrock::BedrockNetworkClientTestAccess::setBeforeTransportInstallHook(
        startDuringClose,
        [&]() {
            startInstallEntered = true;
            while (!releaseStartInstall.load()) std::this_thread::yield();
        }
    );
    startDuringClose.onClose([&]() {
        startCloseEntered = true;
        while (!releaseStartClose.load()) std::this_thread::yield();
        throw std::runtime_error("start-during-close rollback");
    });
    std::thread startCloseThread([&]() {
        try {
            startDuringClose.close("start-close-barrier");
        } catch (const std::exception& error) {
            startCloseEscaped = std::string(error.what()) ==
                "start-during-close rollback";
        }
    });
    const bool reachedStartClose = waitFor(
        [&]() { return startCloseEntered.load(); }
    );
    std::thread lifecycleStarter([&]() {
        (void)startDuringClose.connect();
    });
    const bool startReachedInstall = waitFor(
        [&]() { return startInstallEntered.load(); }
    );
    startDuringClose.queue(
        "client_cache_status",
        bedrock::ProtoDefValue::object({
            {"enabled", bedrock::ProtoDefValue::boolean(false)}
        })
    );
    std::this_thread::sleep_for(120ms);
    const bool startStayedPaused =
        bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
            startDuringClose
        ) == 1 &&
        !bedrock::BedrockNetworkClientTestAccess::queuePumpEnabled(
            startDuringClose
        );
    releaseStartClose = true;
    startCloseThread.join();
    const bool startResumedAfterRollback = waitFor(
        [&]() {
            return bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                startDuringClose
            ) == 0;
        },
        500ms
    );
    releaseStartInstall = true;
    lifecycleStarter.join();
    ok &= check(
        reachedStartClose && startReachedInstall && startStayedPaused &&
            startCloseEscaped.load() && startResumedAfterRollback,
        "startQueue(true) enabled pump inside throwing close snapshot"
    );

    bedrock::BedrockNetworkClient closeRetryClient({
        .host = "127.0.0.1",
        .port = 9,
        .username = "CloseRetry",
        .profile = "CloseRetry",
        .version = "1.20.40",
        .offline = true,
        .batchingIntervalMs = 100,
        .trackWorld = false
    });
    closeRetryClient.onError([](const std::string&) {});
    ok &= check(closeRetryClient.prepareConnectLifecycle(),
                "close retry fixture did not prepare its queue lifecycle");
    closeRetryClient.queue(
        "client_cache_status",
        bedrock::ProtoDefValue::object({
            {"enabled", bedrock::ProtoDefValue::boolean(false)}
        })
    );
    bedrock::BedrockNetworkClientTestAccess::installIdleTransport(
        closeRetryClient
    );
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        closeRetryClient,
        bedrock::ClientStatus::Connecting
    );
    std::atomic<int> closeCalls {0};
    closeRetryClient.onClose([&](const std::string&) {
        const int call = ++closeCalls;
        if (call <= 2) {
            // Longer than one ordinary queue interval. emitClose must pause
            // the native pump for the complete listener snapshot and leave it
            // paused while the exception returns synchronously to this thread.
            if (call == 1) std::this_thread::sleep_for(150ms);
            throw std::runtime_error("close listener boom");
        }
    });
    bool firstCloseEscaped = false;
    try {
        closeRetryClient.close("retry-close");
    } catch (const std::exception& error) {
        firstCloseEscaped = std::string(error.what()) == "close listener boom";
    }
    ok &= check(firstCloseEscaped && closeCalls.load() == 1,
                "throwing close listener did not escape exactly once");
    ok &= check(closeRetryClient.status() == bedrock::ClientStatus::Connecting,
                "throwing close listener changed status");
    ok &= check(
        bedrock::BedrockNetworkClientTestAccess::queueRunning(closeRetryClient),
        "throwing close listener stopped queue lifecycle"
    );
    ok &= check(
        bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
            closeRetryClient
        ) == 1,
        "throwing close listener drained its pending packet"
    );
    ok &= check(
        bedrock::BedrockNetworkClientTestAccess::hasTransport(closeRetryClient),
        "throwing close listener released transport"
    );
    ok &= check(
        !bedrock::BedrockNetworkClientTestAccess::stopRequested(
            closeRetryClient
        ) && !bedrock::BedrockNetworkClientTestAccess::closing(closeRetryClient),
        "throwing close listener committed stop/latch flags"
    );
    bool immediateRetryEscaped = false;
    try {
        // Retry before the first rollback's deferred resume deadline. This
        // frame must carry the logical pump-activity token forward.
        closeRetryClient.close("immediate-retry-close");
    } catch (const std::exception& error) {
        immediateRetryEscaped = std::string(error.what()) ==
            "close listener boom";
    }
    ok &= check(
        immediateRetryEscaped && closeCalls.load() == 2 &&
            bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                closeRetryClient
            ) == 1,
        "back-to-back throwing close retry lost pending queue activity"
    );
    ok &= check(
        waitFor(
            [&]() {
                return bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                    closeRetryClient
                ) == 0;
            },
            1000ms
        ),
        "throwing close listener did not re-arm the queue on a later interval"
    );
    closeRetryClient.queue(
        "client_cache_status",
        bedrock::ProtoDefValue::object({
            {"enabled", bedrock::ProtoDefValue::boolean(false)}
        })
    );
    closeRetryClient.close("retry-close");
    ok &= check(
        closeCalls.load() == 3 &&
            closeRetryClient.status() == bedrock::ClientStatus::Disconnected &&
            !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                closeRetryClient
            ) &&
            bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                closeRetryClient
            ) == 0 &&
            !bedrock::BedrockNetworkClientTestAccess::hasTransport(
                closeRetryClient
            ) &&
            bedrock::BedrockNetworkClientTestAccess::stopRequested(
                closeRetryClient
            ) &&
            !bedrock::BedrockNetworkClientTestAccess::closing(closeRetryClient),
        "second close did not re-emit and complete deferred cleanup"
    );

    bedrock::BedrockNetworkClient recursiveClose({
        .host = "127.0.0.1",
        .port = 9,
        .username = "RecursiveClose",
        .profile = "RecursiveClose",
        .version = "1.20.40",
        .offline = true,
        .trackWorld = false
    });
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        recursiveClose,
        bedrock::ClientStatus::Connecting
    );
    std::vector<std::string> recursiveCloseOrder;
    bool recursed = false;
    bool insideRecursiveClose = false;
    recursiveClose.onClose([&]() {
        if (!recursed) {
            recursiveCloseOrder.push_back("H1_outer");
            recursed = true;
            insideRecursiveClose = true;
            recursiveClose.close("inner-close");
            insideRecursiveClose = false;
        } else {
            recursiveCloseOrder.push_back("H1_inner");
        }
    });
    recursiveClose.onClose([&]() {
        recursiveCloseOrder.push_back(
            insideRecursiveClose ? "H2_inner" : "H2_outer"
        );
    });
    recursiveClose.close("outer-close");
    ok &= check(
        recursiveCloseOrder == std::vector<std::string>({
            "H1_outer", "H1_inner", "H2_inner", "H2_outer"
        }) && recursiveClose.status() ==
            bedrock::ClientStatus::Disconnected,
        "public recursive close did not preserve nested EventEmitter order"
    );

    bedrock::BedrockNetworkClient recursiveRollbackQueue({
        .host = "127.0.0.1",
        .port = 9,
        .username = "RecursiveRollbackQueue",
        .profile = "RecursiveRollbackQueue",
        .version = "1.20.40",
        .offline = true,
        .batchingIntervalMs = 30,
        .trackWorld = false
    });
    recursiveRollbackQueue.onError([](const std::string&) {});
    ok &= check(
        recursiveRollbackQueue.prepareConnectLifecycle(),
        "recursive rollback fixture did not start queue"
    );
    recursiveRollbackQueue.queue(
        "client_cache_status",
        bedrock::ProtoDefValue::object({
            {"enabled", bedrock::ProtoDefValue::boolean(false)}
        })
    );
    bedrock::BedrockNetworkClientTestAccess::installIdleTransport(
        recursiveRollbackQueue
    );
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        recursiveRollbackQueue,
        bedrock::ClientStatus::Connecting
    );
    bool enteredInnerRollback = false;
    bool caughtInnerRollback = false;
    bool packetStayedPausedThroughOuter = false;
    recursiveRollbackQueue.onClose([&]() {
        if (enteredInnerRollback) {
            throw std::runtime_error("inner recursive rollback");
        }
        enteredInnerRollback = true;
        try {
            recursiveRollbackQueue.close("inner-rollback");
        } catch (const std::exception& error) {
            caughtInnerRollback = std::string(error.what()) ==
                "inner recursive rollback";
        }
        // Deliberately keep the outer EventEmitter snapshot active beyond
        // several queue intervals. A failed inner frame must not re-arm the
        // pump until this outer frame itself rolls back or commits.
        std::this_thread::sleep_for(120ms);
        packetStayedPausedThroughOuter =
            bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                recursiveRollbackQueue
            ) == 1 &&
            !bedrock::BedrockNetworkClientTestAccess::queuePumpEnabled(
                recursiveRollbackQueue
            );
    });
    recursiveRollbackQueue.close("outer-after-inner-rollback");
    ok &= check(
        caughtInnerRollback && packetStayedPausedThroughOuter &&
            recursiveRollbackQueue.status() ==
                bedrock::ClientStatus::Disconnected,
        "caught recursive close throw re-armed queue inside outer emission"
    );

    std::atomic<int> destructorThrowCalls {0};
    {
        bedrock::BedrockNetworkClient destructorAfterThrow({
            .host = "127.0.0.1",
            .port = 9,
            .username = "DestructorAfterThrow",
            .profile = "DestructorAfterThrow",
            .version = "1.20.40",
            .offline = true,
            .trackWorld = false
        });
        bedrock::BedrockNetworkClientTestAccess::emitStatus(
            destructorAfterThrow,
            bedrock::ClientStatus::Connecting
        );
        destructorAfterThrow.onClose([&](const std::string&) {
            ++destructorThrowCalls;
            throw std::runtime_error("persistent destructor close boom");
        });
        bool explicitCloseThrew = false;
        try {
            destructorAfterThrow.close("explicit-before-destructor");
        } catch (const std::exception& error) {
            explicitCloseThrew = std::string(error.what()) ==
                "persistent destructor close boom";
        }
        ok &= check(explicitCloseThrew,
                    "persistent close handler did not escape explicit close");
    }
    ok &= check(
        destructorThrowCalls.load() == 1,
        "noexcept destructor re-emitted a throwing close listener"
    );

    bedrock::BedrockNetworkClient concurrentCloseClient({
        .host = "127.0.0.1",
        .port = 9,
        .username = "ConcurrentClose",
        .profile = "ConcurrentClose",
        .version = "1.20.40",
        .offline = true,
        .batchingIntervalMs = 20,
        .trackWorld = false
    });
    concurrentCloseClient.onError([](const std::string&) {});
    ok &= check(concurrentCloseClient.prepareConnectLifecycle(),
                "concurrent close fixture did not prepare queue lifecycle");
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        concurrentCloseClient,
        bedrock::ClientStatus::Connecting
    );
    std::atomic<int> concurrentCloseCalls {0};
    std::atomic<bool> firstCloseEntered {false};
    std::atomic<bool> releaseFirstClose {false};
    std::atomic<bool> firstCloseThrew {false};
    std::atomic<bool> secondCloseReturned {false};
    concurrentCloseClient.onClose([&](const std::string&) {
        if (++concurrentCloseCalls == 1) {
            firstCloseEntered = true;
            while (!releaseFirstClose.load()) std::this_thread::yield();
            throw std::runtime_error("first concurrent close boom");
        }
    });
    std::thread firstCloser([&]() {
        try {
            concurrentCloseClient.close("concurrent-close");
        } catch (const std::exception& error) {
            firstCloseThrew =
                std::string(error.what()) == "first concurrent close boom";
        }
    });
    ok &= check(waitFor([&]() { return firstCloseEntered.load(); }),
                "first concurrent close did not enter listener");
    std::thread secondCloser([&]() {
        concurrentCloseClient.close("concurrent-close");
        secondCloseReturned = true;
    });
    std::this_thread::sleep_for(30ms);
    ok &= check(!secondCloseReturned.load(),
                "concurrent close returned while first emission was active");
    releaseFirstClose = true;
    firstCloser.join();
    secondCloser.join();
    ok &= check(
        firstCloseThrew.load() && secondCloseReturned.load() &&
            concurrentCloseCalls.load() == 2 &&
            concurrentCloseClient.status() ==
                bedrock::ClientStatus::Disconnected,
        "waiting concurrent close did not retry after listener rollback"
    );

    bedrock::BedrockNetworkClient remoteDuringThrow({
        .host = "127.0.0.1",
        .port = 9,
        .username = "RemoteDuringThrow",
        .profile = "RemoteDuringThrow",
        .version = "1.20.40",
        .offline = true,
        .trackWorld = false
    });
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        remoteDuringThrow,
        bedrock::ClientStatus::Connecting
    );
    std::atomic<int> remoteCloseCalls {0};
    std::atomic<bool> publicCloseListenerEntered {false};
    std::atomic<bool> releasePublicCloseListener {false};
    std::atomic<bool> publicCloseEscaped {false};
    std::atomic<bool> remoteCloseReturned {false};
    remoteDuringThrow.onClose([&]() {
        if (++remoteCloseCalls == 1) {
            publicCloseListenerEntered = true;
            while (!releasePublicCloseListener.load()) {
                std::this_thread::yield();
            }
            throw std::runtime_error("slow public close boom");
        }
    });
    std::thread publicCloser([&]() {
        try {
            remoteDuringThrow.close("public-close");
        } catch (const std::exception& error) {
            publicCloseEscaped =
                std::string(error.what()) == "slow public close boom";
        }
    });
    ok &= check(waitFor([&]() { return publicCloseListenerEntered.load(); }),
                "slow public close did not enter listener");
    std::thread remoteCloser([&]() {
        bedrock::BedrockNetworkClientTestAccess::emitTransportClose(
            remoteDuringThrow,
            "remote-close"
        );
        remoteCloseReturned = true;
    });
    std::this_thread::sleep_for(30ms);
    ok &= check(!remoteCloseReturned.load(),
                "independent remote close bypassed active public emission");
    releasePublicCloseListener = true;
    publicCloser.join();
    remoteCloser.join();
    ok &= check(
        publicCloseEscaped.load() && remoteCloseReturned.load() &&
            remoteCloseCalls.load() == 2 &&
            remoteDuringThrow.status() ==
                bedrock::ClientStatus::Disconnected &&
            bedrock::BedrockNetworkClientTestAccess::stopRequested(
                remoteDuringThrow
            ),
        "remote close was lost after throwing public close rollback"
    );

    bedrock::BedrockNetworkClient remoteDuringCommit({
        .host = "127.0.0.1",
        .port = 9,
        .username = "RemoteDuringCommit",
        .profile = "RemoteDuringCommit",
        .version = "1.20.40",
        .offline = true,
        .trackWorld = false
    });
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        remoteDuringCommit,
        bedrock::ClientStatus::Connecting
    );
    std::atomic<int> committedCloseCalls {0};
    std::atomic<bool> committedListenerEntered {false};
    std::atomic<bool> releaseCommittedListener {false};
    std::atomic<bool> committedPublicReturned {false};
    std::atomic<bool> committedRemoteReturned {false};
    remoteDuringCommit.onClose([&]() {
        ++committedCloseCalls;
        committedListenerEntered = true;
        while (!releaseCommittedListener.load()) std::this_thread::yield();
    });
    std::thread committedPublic([&]() {
        remoteDuringCommit.close("committed-public-close");
        committedPublicReturned = true;
    });
    ok &= check(waitFor([&]() { return committedListenerEntered.load(); }),
                "committed public close did not enter listener");
    std::thread committedRemote([&]() {
        bedrock::BedrockNetworkClientTestAccess::emitTransportClose(
            remoteDuringCommit,
            "concurrent-remote-close"
        );
        committedRemoteReturned = true;
    });
    std::this_thread::sleep_for(30ms);
    ok &= check(!committedRemoteReturned.load(),
                "remote close returned before public commit publication");
    releaseCommittedListener = true;
    committedPublic.join();
    committedRemote.join();
    ok &= check(
        committedPublicReturned.load() && committedRemoteReturned.load() &&
            committedCloseCalls.load() == 1 &&
            remoteDuringCommit.status() ==
                bedrock::ClientStatus::Disconnected,
        "transport waiter did not drop after successful public commit"
    );

    bedrock::BedrockNetworkClient closeDuringPrepare({
        .host = "127.0.0.1",
        .port = 9,
        .username = "CloseDuringPrepare",
        .profile = "CloseDuringPrepare",
        .version = "1.20.40",
        .offline = true,
        .batchingIntervalMs = 20,
        .trackWorld = false
    });
    std::atomic<bool> queueStartHookEntered {false};
    std::atomic<bool> releaseQueueStartHook {false};
    std::atomic<bool> prepareResult {true};
    bedrock::BedrockNetworkClientTestAccess::setBeforeQueueStartHook(
        closeDuringPrepare,
        [&]() {
            queueStartHookEntered = true;
            while (!releaseQueueStartHook.load()) std::this_thread::yield();
        }
    );
    std::thread preparer([&]() {
        prepareResult = closeDuringPrepare.prepareConnectLifecycle();
    });
    const bool reachedQueueStart = waitFor(
        [&]() { return queueStartHookEntered.load(); }
    );
    ok &= check(reachedQueueStart,
                "prepare/close fixture did not reach queue-start barrier");
    if (reachedQueueStart) {
        closeDuringPrepare.close("close-during-prepare");
        closeDuringPrepare.queue(
            "client_cache_status",
            bedrock::ProtoDefValue::object({
                {"enabled", bedrock::ProtoDefValue::boolean(false)}
            })
        );
    }
    releaseQueueStartHook = true;
    preparer.join();
    ok &= check(
        reachedQueueStart && !prepareResult.load() &&
            bedrock::BedrockNetworkClientTestAccess::connectLifecycleIdle(
                closeDuringPrepare
            ) &&
            !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                closeDuringPrepare
            ) &&
            bedrock::BedrockNetworkClientTestAccess::queuedPacketCount(
                closeDuringPrepare
            ) == 1,
        "preparation resurrected a queue or dropped post-close packet"
    );
    closeDuringPrepare.close("prepare-race-cleanup");

    bedrock::BedrockNetworkClient closeBeforeTransportInstall({
        .host = "127.0.0.1",
        .port = 9,
        .username = "CloseBeforeTransportInstall",
        .profile = "CloseBeforeTransportInstall",
        .version = "1.20.40",
        .offline = true,
        .connectTimeoutMs = 100,
        .trackWorld = false
    });
    closeBeforeTransportInstall.onError([](const std::string&) {});
    std::atomic<bool> beforeInstallEntered {false};
    std::atomic<bool> releaseBeforeInstall {false};
    std::atomic<bool> installRaceConnectResult {true};
    bedrock::BedrockNetworkClientTestAccess::setBeforeTransportInstallHook(
        closeBeforeTransportInstall,
        [&]() {
            beforeInstallEntered = true;
            while (!releaseBeforeInstall.load()) std::this_thread::yield();
        }
    );
    std::thread installRaceConnector([&]() {
        installRaceConnectResult = closeBeforeTransportInstall.connect();
    });
    const bool reachedBeforeInstall = waitFor(
        [&]() { return beforeInstallEntered.load(); }
    );
    ok &= check(reachedBeforeInstall,
                "transport install race did not reach pre-install barrier");
    closeBeforeTransportInstall.close("close-before-transport-install");
    ok &= check(
        !bedrock::BedrockNetworkClientTestAccess::hasTransport(
            closeBeforeTransportInstall
        ),
        "close returned with transport unexpectedly installed"
    );
    releaseBeforeInstall = true;
    installRaceConnector.join();
    ok &= check(
        !installRaceConnectResult.load() &&
            !bedrock::BedrockNetworkClientTestAccess::hasTransport(
                closeBeforeTransportInstall
            ),
        "connect installed/retained transport after close returned"
    );

    bedrock::BedrockNetworkClient reentrantConnect({
        .host = "127.0.0.1",
        .port = 9,
        .username = "ReentrantConnect",
        .profile = "ReentrantConnect",
        .version = "1.20.40",
        .offline = true,
        .authCacheRoot = std::filesystem::current_path() / "CMakeLists.txt",
        .trackWorld = false,
        .loginPacket = {0x01}
    });
    std::atomic<int> reentrantErrors {0};
    std::atomic<bool> nestedConnectReturned {false};
    std::atomic<bool> nestedConnectResult {true};
    reentrantConnect.onError([&](const std::string&) {
        ++reentrantErrors;
        nestedConnectResult = reentrantConnect.connect();
        nestedConnectReturned = true;
    });
    const bool outerConnectResult = reentrantConnect.connect();
    ok &= check(
        !outerConnectResult && reentrantErrors.load() == 1 &&
            nestedConnectReturned.load() && !nestedConnectResult.load(),
        "same-thread onError connect retry deadlocked or started recursively"
    );

    // The network emitter owns one admission lease and one listener snapshot
    // for the complete emission. Destroying the public facade in listener 1
    // must not destroy `this` or skip listener 2.
    std::unique_ptr<bedrock::Client> owner(
        new bedrock::Client(baseOptions(9))
    );
    auto* network = &owner->network();
    std::vector<int> order;
    owner->onError([&](const std::string&) {
        order.push_back(1);
        owner.reset();
    });
    owner->onError([&](const std::string&) { order.push_back(2); });
    bedrock::BedrockNetworkClientTestAccess::emitError(*network, "snapshot");
    ok &= check(order == std::vector<int>({1, 2}),
                "owner destruction invalidated the active error snapshot");

    bedrock::BedrockNetworkClient callbackClient;
    bedrock::BedrockNetworkClientTestAccess::enterRakNetCallback(callbackClient);
    ok &= check(callbackClient.onRakNetCallbackThread(),
                "RakNet callback scope did not mark active thread");
    bedrock::BedrockNetworkClientTestAccess::enterRakNetCallback(callbackClient);
    bedrock::BedrockNetworkClientTestAccess::leaveRakNetCallback(callbackClient);
    ok &= check(callbackClient.onRakNetCallbackThread(),
                "nested RakNet callback cleared outer callback depth");
    bedrock::BedrockNetworkClientTestAccess::leaveRakNetCallback(callbackClient);
    ok &= check(!callbackClient.onRakNetCallbackThread(),
                "RakNet callback thread id remained live after scope exit");
    std::atomic<bool> staleThreadId {true};
    std::thread idProbe([&]() {
        staleThreadId = callbackClient.onRakNetCallbackThread();
    });
    idProbe.join();
    ok &= check(!staleThreadId.load(),
                "inactive callback id matched a later thread");

    // Mutations, provider replacement and snapshots share one mutex, but no
    // user callback/provider executes under it.
    bedrock::BedrockNetworkClient concurrentClient;
    auto token = std::make_shared<int>(1);
    auto provider = [token]() -> std::shared_ptr<void> { return token; };
    concurrentClient.setCallbackLifetimeProvider(provider);
    std::atomic<int> statusCalls {0};
    concurrentClient.onStatus([&](bedrock::ClientStatus) { ++statusCalls; });
    std::thread registrar([&]() {
        for (int i = 0; i < 200; ++i) {
            concurrentClient.onStatus(
                [&](bedrock::ClientStatus) { ++statusCalls; }
            );
        }
    });
    std::thread emitter([&]() {
        for (int i = 0; i < 200; ++i) {
            bedrock::BedrockNetworkClientTestAccess::emitStatus(
                concurrentClient,
                bedrock::ClientStatus::Connecting
            );
        }
    });
    std::thread mutator([&]() {
        for (int i = 0; i < 100; ++i) {
            concurrentClient.setCallbackLifetimeProvider(provider);
            bedrock::BedrockNetworkClientTestAccess::clearHandlers(
                concurrentClient
            );
            concurrentClient.onStatus(
                [&](bedrock::ClientStatus) { ++statusCalls; }
            );
        }
    });
    registrar.join();
    emitter.join();
    mutator.join();
    concurrentClient.setCallbackLifetimeProvider(provider);
    const auto beforeFinalStatus = statusCalls.load();
    concurrentClient.onStatus([&](bedrock::ClientStatus) { ++statusCalls; });
    bedrock::BedrockNetworkClientTestAccess::emitStatus(
        concurrentClient,
        bedrock::ClientStatus::Initialized
    );
    ok &= check(statusCalls.load() > beforeFinalStatus,
                "concurrent handler/provider mutation lost final emission");

    bedrock::BedrockNetworkClient connectedBoundary;
    std::atomic<int> convertedClientErrors {0};
    connectedBoundary.onError(
        [&](const std::string&) { ++convertedClientErrors; }
    );
    connectedBoundary.onStatus([](bedrock::ClientStatus) {
        throw std::runtime_error("connected listener boom");
    });
    bool connectedThrowEscaped = false;
    try {
        bedrock::BedrockNetworkClientTestAccess::handleConnected(
            connectedBoundary
        );
    } catch (const std::exception& e) {
        connectedThrowEscaped =
            std::string(e.what()) == "connected listener boom";
    }
    ok &= check(connectedThrowEscaped && convertedClientErrors.load() == 0,
                "live connected exception was converted into Client error");

    // Native parse failures remain contained, while exceptions from the live
    // encapsulated callback cross the callback boundary instead of being
    // silently converted into a transport parse error.
    bedrock::RakNetClient throwingRakNet;
    throwingRakNet.onEncapsulated([](const std::vector<uint8_t>&) {
        throw std::runtime_error("live listener boom");
    });
    bool liveThrowEscaped = false;
    try {
        bedrock::RakNetClientTestAccess::dispatch(
            throwingRakNet,
            connectedDatagram(1, {0xfe})
        );
    } catch (const std::exception& e) {
        liveThrowEscaped = std::string(e.what()) == "live listener boom";
    }
    ok &= check(liveThrowEscaped,
                "RakNet swallowed an exception from a live listener");
    bool malformedEscaped = false;
    try {
        bedrock::RakNetClientTestAccess::dispatch(throwingRakNet, {0x80});
    } catch (...) {
        malformedEscaped = true;
    }
    ok &= check(!malformedEscaped && !throwingRakNet.error().empty(),
                "native RakNet parse error crossed the callback boundary");

    bedrock::RakNetClient disconnectRakNet;
    std::vector<std::string> closeReasons;
    std::atomic<int> leakedPayloads {0};
    disconnectRakNet.onClose([&](const std::string& reason) {
        closeReasons.push_back(reason);
    });
    disconnectRakNet.onEncapsulated(
        [&](const std::vector<uint8_t>&) { ++leakedPayloads; }
    );
    const std::vector<uint8_t> disconnectIds {21, 22, 23, 25};
    for (std::size_t i = 0; i < disconnectIds.size(); ++i) {
        bedrock::RakNetClientTestAccess::dispatch(
            disconnectRakNet,
            connectedDatagram(
                static_cast<uint32_t>(10 + i),
                {disconnectIds[i]}
            )
        );
    }
    ok &= check(
        closeReasons == std::vector<std::string>({"21", "22", "23", "25"}) &&
            leakedPayloads.load() == 0,
        "RakNet disconnect MessageIDs leaked into the MCPE callback"
    );
    return ok;
}

bool checkRakNetCloseAndRealCallbackDestruction() {
    bool ok = true;
    bedrock::BedrockServer server({
        .host = "127.0.0.1",
        .port = 0,
        .version = "1.20.40",
        .motd = {{"motd", "Factory Lifetime Smoke"}},
        .maxPlayers = 64
    });
    server.listen();

    {
        bedrock::BedrockNetworkClient concurrentConnect({
            .host = "127.0.0.1",
            .port = server.boundPort(),
            .username = "ConcurrentConnect",
            .profile = "ConcurrentConnect",
            .version = "1.20.40",
            .offline = true,
            .connectTimeoutMs = 500,
            .trackWorld = false
        });
        concurrentConnect.onError([](const std::string&) {});
        std::atomic<int> transportInstalls {0};
        std::atomic<bool> installHookEntered {false};
        std::atomic<bool> releaseInstallHook {false};
        std::atomic<bool> firstConnectResult {false};
        std::atomic<bool> secondConnectResult {false};
        std::atomic<bool> secondConnectReturned {false};
        bedrock::BedrockNetworkClientTestAccess::setAfterTransportInstallHook(
            concurrentConnect,
            [&]() {
                ++transportInstalls;
                installHookEntered = true;
                while (!releaseInstallHook.load()) std::this_thread::yield();
            }
        );
        std::thread firstConnector([&]() {
            firstConnectResult = concurrentConnect.connect();
        });
        ok &= check(waitFor([&]() { return installHookEntered.load(); }),
                    "concurrent connect owner did not install transport");
        std::thread secondConnector([&]() {
            secondConnectResult = concurrentConnect.connect();
            secondConnectReturned = true;
        });
        std::this_thread::sleep_for(30ms);
        ok &= check(!secondConnectReturned.load(),
                    "second connect bypassed active lifecycle owner");
        releaseInstallHook = true;
        firstConnector.join();
        secondConnector.join();
        ok &= check(
            firstConnectResult.load() && secondConnectResult.load() &&
                transportInstalls.load() == 1,
            "concurrent connect installed more than one RakNet transport"
        );
        concurrentConnect.close("concurrent-connect-cleanup");
    }

    {
        bedrock::BedrockNetworkClient closeAtPhaseCommit({
            .host = "127.0.0.1",
            .port = server.boundPort(),
            .username = "CloseAtPhaseCommit",
            .profile = "CloseAtPhaseCommit",
            .version = "1.20.40",
            .offline = true,
            .connectTimeoutMs = 500,
            .trackWorld = false
        });
        closeAtPhaseCommit.onError([](const std::string&) {});
        std::atomic<bool> phaseHookEntered {false};
        std::atomic<bool> releasePhaseHook {false};
        std::atomic<bool> connectResult {false};
        bedrock::BedrockNetworkClientTestAccess::setBeforeConnectPhaseCommitHook(
            closeAtPhaseCommit,
            [&]() {
                phaseHookEntered = true;
                while (!releasePhaseHook.load()) std::this_thread::yield();
            }
        );
        std::thread connector([&]() {
            connectResult = closeAtPhaseCommit.connect();
        });
        const bool reachedPhaseHook = waitFor(
            [&]() { return phaseHookEntered.load(); }, 800ms
        );
        ok &= check(reachedPhaseHook,
                    "successful connect did not reach phase-commit hook");
        if (!reachedPhaseHook) releasePhaseHook = true;
        closeAtPhaseCommit.close("close-before-phase-commit");
        releasePhaseHook = true;
        connector.join();
        ok &= check(
            connectResult.load() &&
                !bedrock::BedrockNetworkClientTestAccess::connectLifecycleActive(
                    closeAtPhaseCommit
                ) &&
                bedrock::BedrockNetworkClientTestAccess::stopRequested(
                    closeAtPhaseCommit
                ) &&
                closeAtPhaseCommit.status() ==
                    bedrock::ClientStatus::Disconnected,
            "close racing successful transport return resurrected Active phase"
        );
    }

    {
        std::unique_ptr<bedrock::Client> callbackOwner(
            new bedrock::Client(baseOptions(9))
        );
        std::atomic<bool> decisionHookEntered {false};
        std::atomic<bool> releaseDecisionHook {false};
        std::atomic<int> remainingStatusSnapshot {0};
        ok &= check(callbackOwner->network().prepareConnectLifecycle(),
                    "callback-destruction fixture did not start queue");
        bedrock::ClientFactoryTestAccess::installNoCloseFinalizingWorker(
            *callbackOwner,
            decisionHookEntered,
            releaseDecisionHook
        );
        callbackOwner->onStatus([&](bedrock::ClientStatus status) {
            if (status == bedrock::ClientStatus::Initialized) {
                callbackOwner.reset();
            }
        });
        callbackOwner->onStatus([&](bedrock::ClientStatus status) {
            if (status == bedrock::ClientStatus::Initialized) {
                ++remainingStatusSnapshot;
            }
        });
        const bool reachedDecisionHook = waitFor(
            [&]() { return decisionHookEntered.load(); }
        );
        ok &= check(reachedDecisionHook,
                    "synthetic worker did not reach post-decision barrier");
        auto callbackNetwork = reachedDecisionHook
            ? bedrock::ClientFactoryTestAccess::sharedNetwork(*callbackOwner)
            : std::shared_ptr<bedrock::BedrockNetworkClient>();
        auto callbackState = reachedDecisionHook
            ? bedrock::ClientFactoryTestAccess::weakState(*callbackOwner)
            : std::weak_ptr<void>();
        if (callbackNetwork) {
            bedrock::BedrockNetworkClientTestAccess::emitStatusAsRakNetCallback(
                *callbackNetwork,
                bedrock::ClientStatus::Initialized
            );
        }
        ok &= check(
            !callbackOwner && remainingStatusSnapshot.load() == 1,
            "callback destruction skipped remaining status snapshot"
        );
        releaseDecisionHook = true;
        ok &= check(
            waitFor([&]() { return callbackState.expired(); }, 1000ms),
            "post-decision callback destruction retained factory State"
        );
        ok &= check(
            callbackNetwork && callbackNetwork->status() ==
                bedrock::ClientStatus::Disconnected,
            "post-decision callback destruction retained network status"
        );
        ok &= check(
            callbackNetwork &&
                !bedrock::BedrockNetworkClientTestAccess::queueRunning(
                    *callbackNetwork
                ),
            "post-decision callback destruction retained queue thread"
        );
    }

    {
        bedrock::RakNetClient source({
            .host = "127.0.0.1",
            .port = server.boundPort(),
            .mtu = 1400,
            .protocolVersion = 11,
            .timeoutMs = 500
        });
        bedrock::RakNetClient moved(std::move(source));
        std::atomic<bool> movedConnected {false};
        moved.onConnected([&]() { movedConnected = true; });
        ok &= check(moved.connect(),
                    "fresh RakNet move lost reusable connect state");
        ok &= check(waitFor([&]() { return movedConnected.load(); }, 800ms),
                    "moved RakNet client did not reach connected callback");
        moved.close("moved-client-cleanup");
    }

    {
        bedrock::RakNetClient replacement({
            .host = "127.0.0.1",
            .port = server.boundPort(),
            .mtu = 1400,
            .protocolVersion = 11,
            .timeoutMs = 500
        });
        auto client = std::make_unique<bedrock::RakNetClient>(
            bedrock::RakNetClientOptions {
                .host = "127.0.0.1",
                .port = server.boundPort(),
                .mtu = 1400,
                .protocolVersion = 11,
                .timeoutMs = 500
            }
        );
        std::atomic<bool> activeMoveRejected {false};
        std::atomic<bool> activeDestinationRejected {false};
        std::atomic<bool> originalSurvived {false};
        std::atomic<bool> activeMoveCallbackDone {false};
        std::string activeMoveError;
        std::string activeDestinationError;
        client->onConnected([&]() {
            try {
                bedrock::RakNetClient invalid(std::move(*client));
            } catch (const std::logic_error& error) {
                activeMoveError = error.what();
                activeMoveRejected = true;
            }
            try {
                *client = std::move(replacement);
            } catch (const std::logic_error& error) {
                activeDestinationError = error.what();
                activeDestinationRejected = true;
            }
            originalSurvived = client->connected();
            activeMoveCallbackDone = true;
        });
        ok &= check(client->connect(),
                    "active-move RakNet handshake failed");
        ok &= check(
            waitFor([&]() { return activeMoveCallbackDone.load(); }, 800ms),
            "active RakNet move callback did not complete"
        );
        ok &= check(
            activeMoveError == "Cannot move active RakNetClient" &&
                originalSurvived.load(),
            "active RakNet move mutated the live source before rejection"
        );
        ok &= check(
            activeDestinationRejected.load() &&
                activeDestinationError == "Cannot move active RakNetClient",
            "move assignment into an active RakNet destination was not rejected"
        );
        client->close("active-move-cleanup");

        std::atomic<bool> replacementConnected {false};
        replacement.onConnected([&]() { replacementConnected = true; });
        ok &= check(replacement.connect(),
                    "rejected active-destination move mutated its fresh source");
        ok &= check(
            waitFor([&]() { return replacementConnected.load(); }, 800ms),
            "fresh source did not connect after rejected move assignment"
        );
        replacement.close("active-destination-source-cleanup");
    }

    for (int iteration = 0; iteration < 8; ++iteration) {
        bedrock::RakNetClient commitClient({
            .host = "127.0.0.1",
            .port = server.boundPort(),
            .mtu = 1400,
            .protocolVersion = 11,
            .timeoutMs = 500
        });
        std::atomic<bool> commitWindowEntered {false};
        std::atomic<bool> releaseCommit {false};
        std::atomic<bool> connectResult {true};
        bedrock::RakNetClientTestAccess::setBeforeRunningCommitHook(
            commitClient,
            [&]() {
                commitWindowEntered = true;
                while (!releaseCommit.load()) std::this_thread::yield();
            }
        );
        std::thread connector([&]() {
            connectResult = commitClient.connect();
        });
        const bool entered = waitFor(
            [&]() { return commitWindowEntered.load(); },
            800ms
        );
        commitClient.requestStop();
        releaseCommit = true;
        connector.join();
        ok &= check(entered && !connectResult.load() &&
                        !commitClient.connected(),
                    "stop in RakNet running-commit window was overwritten");
        commitClient.close("commit-window-cleanup");
    }

    for (int iteration = 0; iteration < 8; ++iteration) {
        bedrock::RakNetClient client({
            .host = "127.0.0.1",
            .port = server.boundPort(),
            .mtu = 1400,
            .protocolVersion = 11,
            .timeoutMs = 500
        });
        std::atomic<bool> connected {false};
        client.onConnected([&]() { connected = true; });
        ok &= check(client.connect(),
                    "RakNet close stress could not complete handshake");
        ok &= check(waitFor([&]() { return connected.load(); }, 800ms),
                    "RakNet close stress missed connected callback");
        std::thread first([&]() { client.close("stress-1"); });
        std::thread second([&]() { client.close("stress-2"); });
        std::thread third([&]() { client.close("stress-3"); });
        first.join();
        second.join();
        third.join();
    }

    {
        auto client = std::make_unique<bedrock::RakNetClient>(
            bedrock::RakNetClientOptions {
                .host = "127.0.0.1",
                .port = server.boundPort(),
                .mtu = 1400,
                .protocolVersion = 11,
                .timeoutMs = 500
            }
        );
        std::atomic<bool> selfCloseReturned {false};
        client->onConnected([&]() {
            client->close("self-callback-close");
            selfCloseReturned = true;
        });
        ok &= check(client->connect(),
                    "self-close RakNet handshake failed");
        ok &= check(waitFor([&]() { return selfCloseReturned.load(); }, 800ms),
                    "close from RakNet callback joined itself");
        client.reset();
    }

    // Force the factory connect worker to remain active while a real inbound
    // RakNet status callback destroys the public facade. shutdown must not
    // join that worker from the callback thread, and listener 2 must remain in
    // the already-admitted EventEmitter snapshot.
    auto options = baseOptions(server.boundPort());
    options.followPort = false;
    options.connectTimeout = 500;
    std::unique_ptr<bedrock::Client> owner(
        new bedrock::Client(bedrock::createClient(options))
    );
    std::atomic<bool> destroyedInCallback {false};
    std::atomic<bool> remainingStatusListener {false};
    std::atomic<bool> connectHookEntered {false};
    bedrock::ClientFactoryTestAccess::setAfterNetworkConnectHook(
        *owner,
        [&]() {
            connectHookEntered = true;
            (void) waitFor(
                [&]() { return destroyedInCallback.load(); },
                800ms
            );
        }
    );
    owner->onError([](const std::string&) {});
    owner->onStatus([&](bedrock::ClientStatus status) {
        if (status == bedrock::ClientStatus::Connecting && owner) {
            owner.reset();
            destroyedInCallback = true;
        }
    });
    owner->onStatus([&](bedrock::ClientStatus status) {
        if (status == bedrock::ClientStatus::Connecting) {
            remainingStatusListener = true;
        }
    });
    owner->onConnectAllowed([]() {});
    ok &= check(waitFor([&]() { return destroyedInCallback.load(); }, 1200ms),
                "real RakNet callback destruction deadlocked");
    ok &= check(waitFor([&]() { return remainingStatusListener.load(); }, 500ms),
                "real callback destruction skipped remaining status listener");
    ok &= check(waitFor([&]() { return connectHookEntered.load(); }, 500ms),
                "forced active-connect barrier was not exercised");
    std::this_thread::sleep_for(50ms);
    server.close();
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= checkDirectAndSkipPing();
    ok &= checkCanonicalClientOptionsParity();
    ok &= checkAsyncOrderAndFourPartVersion();
    ok &= checkSlowConnectAllowedQueueGate();
    ok &= checkDiscoverySelection();
    ok &= checkCloseInsideConnectAllowed();
    ok &= checkThrowingConnectAllowed();
    ok &= checkPrototypeVersionAdvertisement();
    ok &= checkExplicitEmptyVersionAdvertisement();
    ok &= checkPingErrorAndCloseBeforePong();
    ok &= checkInvalidAuthenticationBoundary();
    ok &= checkSuppliedAuthflowBoundary();
    ok &= checkEventLifetimeAndRakNetBoundaries();
    ok &= checkRakNetCloseAndRealCallbackDestruction();
    if (!ok) return 1;
    std::cout << "[CREATE-CLIENT-FACTORY-SMOKE] ok\n";
    return 0;
}
