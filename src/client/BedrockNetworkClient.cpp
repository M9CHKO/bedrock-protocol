#include <bedrock/client/BedrockNetworkClient.hpp>

#include <bedrock/auth/BedrockAuthJwt.hpp>
#include <bedrock/auth/BedrockClientDataBuilder.hpp>
#include <bedrock/auth/NativeBedrockAuthflow.hpp>
#include <bedrock/protocol/ProtocolDefinition.hpp>
#include <bedrock/protodef/ProtoDefPacketDecoder.hpp>
#include <bedrock/world/BedrockSubChunkPacket.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace bedrock {

namespace {

template <typename Function>
class NetworkScopeExit {
public:
    explicit NetworkScopeExit(Function function)
        : function_(std::move(function)) {}

    NetworkScopeExit(const NetworkScopeExit&) = delete;
    NetworkScopeExit& operator=(const NetworkScopeExit&) = delete;

    ~NetworkScopeExit() {
        function_();
    }

private:
    Function function_;
};

template <typename Function>
NetworkScopeExit<Function> networkScopeExit(Function function) {
    return NetworkScopeExit<Function>(std::move(function));
}

bool isObjectPrototypeVersionName(std::string_view version) {
    for (const std::string_view inherited : {
             "constructor",
             "__defineGetter__",
             "__defineSetter__",
             "hasOwnProperty",
             "__lookupGetter__",
             "__lookupSetter__",
             "isPrototypeOf",
             "propertyIsEnumerable",
             "toString",
             "valueOf",
             "__proto__",
             "toLocaleString"
         }) {
        if (version == inherited) {
            return true;
        }
    }
    return false;
}

std::optional<uint32_t> connectionComparisonProtocolVersion(
    const std::string& version
) {
    if (const auto* entry = findVersion(version)) {
        return entry->protocolVersion;
    }
    if (isObjectPrototypeVersionName(version)) {
        return std::nullopt;
    }
    throw std::runtime_error("Unknown version: " + version);
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

std::string findFieldValue(
    const std::vector<ProtoDefField>& fields,
    const std::string& name
) {
    for (const auto& field : fields) {
        if (field.path == name) return field.value;
        auto dot = field.path.rfind('.');
        if (dot != std::string::npos && field.path.substr(dot + 1) == name) {
            return field.value;
        }
    }
    return {};
}

uint16_t parseU16Field(
    const std::vector<ProtoDefField>& fields,
    const std::string& name,
    uint16_t fallback
) {
    auto value = findFieldValue(fields, name);
    if (value.empty()) return fallback;
    try {
        return static_cast<uint16_t>(std::stoul(value));
    } catch (const std::exception&) {
        return fallback;
    }
}

int32_t playStatusCode(const VersionedGamePacket& packet) {
    if (packet.name != "play_status" || packet.payload.size() < 4) {
        return -1;
    }

    return (static_cast<int32_t>(packet.payload[0]) << 24) |
        (static_cast<int32_t>(packet.payload[1]) << 16) |
        (static_cast<int32_t>(packet.payload[2]) << 8) |
        static_cast<int32_t>(packet.payload[3]);
}

int64_t readI64LE(const std::vector<uint8_t>& data, std::size_t offset) {
    if (offset + 8 > data.size()) {
        throw std::runtime_error("not enough bytes for li64");
    }

    uint64_t raw = 0;
    for (int i = 0; i < 8; ++i) {
        raw |= static_cast<uint64_t>(data[offset + static_cast<std::size_t>(i)]) << (i * 8);
    }
    return static_cast<int64_t>(raw);
}

int64_t unixTimeMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

class AuthflowPromiseRejection final : public std::runtime_error {
public:
    explicit AuthflowPromiseRejection(const std::string& message)
        : std::runtime_error(message) {}
};

void dispatchAuthenticationUnhandledRejection(
    BedrockNetworkClient::AuthenticationUnhandledRejectionHandler handler,
    std::string message
) {
    if (!handler) return;
    // An ignored async authenticate() Promise is reported only after the
    // synchronous Client.connect()/startQueue stack and the current microtask
    // checkpoint. C++ has no owning JS event loop, so hand the already-owned
    // callback/message to a detached native turn; it never captures the client.
    std::thread([
        handler = std::move(handler),
        message = std::move(message)
    ]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        try {
            handler(std::move(message));
        } catch (...) {
            // This is a process-boundary reporting seam. A throwing observer
            // cannot retroactively change the ignored Promise or queue start.
        }
    }).detach();
}

} // namespace

BedrockNetworkClient::BedrockNetworkClient(BedrockNetworkClientOptions options)
    : options_(normalizeOptions(std::move(options))),
      session_(VersionedClientSessionOptions{
          .minecraftVersion = options_.version,
          .outgoingCompression = VersionedMcpeCompression::DeflateRaw,
          .autoResourcePackResponses = false,
          .autoStartGameInit = false,
          .clientCacheEnabled = options_.clientCacheEnabled,
          .chunkRadius = options_.chunkRadius
      }),
      packetVariables_(makeProtoDefVariableStore()),
      packetEncoder_(options_.version, packetVariables_),
      packetDecoder_(options_.version, packetVariables_) {
    compressionAlgorithm_ = versionAtLeast(options_.version, 1, 19, 30) ? "none" : "deflate";
}

BedrockNetworkClient::~BedrockNetworkClient() noexcept {
    // A C++ destructor cannot propagate EventEmitter-style listener
    // exceptions. Remove user callbacks before the final close so a listener
    // that aborted an earlier explicit close is not invoked again from this
    // noexcept boundary.
    try {
        clearEventHandlers();
        close();
        return;
    } catch (...) {
        // Fall through to callback-free best-effort teardown. No exception may
        // cross the destructor boundary.
    }

    rakNetStopRequested_.store(true);
    try { stopQueue(); } catch (...) {}

    std::shared_ptr<RakNetClient> transport;
    try {
        std::lock_guard<std::mutex> lock(sendMutex_);
        transport = std::move(raknet_);
    } catch (...) {}
    if (transport) {
        try { transport->close("closed"); } catch (...) {}
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = BedrockNetworkClientStatus::Disconnected;
    } catch (...) {}
    closed_.store(true);
    closedCv_.notify_all();
}

BedrockNetworkClient::RakNetCallbackScope::RakNetCallbackScope(
    BedrockNetworkClient& owner
) : owner_(owner) {
    owner_.enterRakNetCallback();
}

BedrockNetworkClient::RakNetCallbackScope::~RakNetCallbackScope() {
    owner_.leaveRakNetCallback();
}

bool BedrockNetworkClient::connect() {
    bool authenticationRejected = false;
    for (;;) {
        std::unique_lock<std::mutex> lock(connectLifecycleMutex_);
        if (rakNetStopRequested_.load()) return false;

        if (connectLifecyclePhase_ == ConnectLifecyclePhase::Active) {
            return true;
        }
        if (connectLifecyclePhase_ == ConnectLifecyclePhase::Connecting) {
            if (connectLifecycleOwnerThreadId_ ==
                std::this_thread::get_id()) {
                // A live error/status callback may synchronously retry
                // connect() on the thread that owns this very attempt. Waiting
                // for ourselves would deadlock; the in-flight attempt has not
                // become Active and the reentrant call observes false.
                return false;
            }
            connectLifecycleCv_.wait(lock, [this]() {
                return connectLifecyclePhase_ !=
                    ConnectLifecyclePhase::Connecting;
            });
            return connectLifecyclePhase_ == ConnectLifecyclePhase::Active;
        }
        if (connectLifecyclePhase_ == ConnectLifecyclePhase::Preparing) {
            if (connectLifecycleOwnerThreadId_ ==
                std::this_thread::get_id()) {
                // The facade has just completed a same-stack recursive
                // preparation. JavaScript returns from the nested connect()
                // after its startQueue(); transport admission remains the
                // outer async/session continuation's responsibility.
                return false;
            }
            connectLifecycleCv_.wait(lock, [this]() {
                return connectLifecyclePhase_ !=
                    ConnectLifecyclePhase::Preparing;
            });
            continue;
        }
        if (connectLifecyclePhase_ == ConnectLifecyclePhase::Idle) {
            lock.unlock();
            if (!prepareConnectLifecycle(false)) return false;
            continue;
        }

        connectLifecyclePhase_ = ConnectLifecyclePhase::Connecting;
        connectLifecycleOwnerThreadId_ = std::this_thread::get_id();
        authenticationRejected = authenticationRejected_;
        break;
    }

    bool connectedSuccessfully = false;
    auto phaseCompletion = networkScopeExit(
        [this, &connectedSuccessfully]() {
            {
                std::lock_guard<std::mutex> lock(connectLifecycleMutex_);
                // Read terminal state while holding the same phase lock used
                // by emitClose(). A close racing successful transport return
                // must not be overwritten by a stale pre-lock `true` value.
                const bool remainActive = connectedSuccessfully &&
                    !rakNetStopRequested_.load() && !closed_.load();
                connectLifecyclePhase_ = remainActive
                    ? ConnectLifecyclePhase::Active
                    : ConnectLifecyclePhase::Idle;
                connectLifecycleOwnerThreadId_ = std::thread::id{};
            }
            connectLifecycleCv_.notify_all();
        }
    );

    // The JavaScript timer becomes runnable only after the current
    // connect_allowed EventEmitter stack returns. Factory initialization
    // prepares a paused queue; the single connect owner admits it here.
    resumeQueuePump();
    if (rakNetStopRequested_.load()) {
        stopQueue();
        return false;
    }

    // auth.authenticate() catches a rejected Authflow constructor and never
    // emits `session`. Client.connect() has already called startQueue(), but
    // _connect/transport is therefore never reached.
    if (authenticationRejected) {
        return false;
    }

    // Online authentication is promise-driven in JavaScript and therefore
    // cannot delay later connect_allowed listeners. Native authentication may
    // block, so it belongs to this tracked connect worker, never the listener
    // snapshot's synchronous preparation phase.
    try {
        prepareLoginPacket();
    } catch (const std::exception& e) {
        const auto currentOptions = options();
        const bool promiseRejected =
            dynamic_cast<const AuthflowPromiseRejection*>(&e) != nullptr;
        if (!currentOptions.password.empty() &&
            (!currentOptions.authflow || promiseRejected)) {
            std::cerr
                << "Sign in failed, try removing the password field\n";
        }
        // authenticate() logs the original failure and emits `error`, but its
        // catch swallows a normally handled event. startQueue has already run
        // and remains alive unless an error listener explicitly closes it.
        std::cerr << e.what() << "\n";

        AuthenticationUnhandledRejectionHandler rejectionHandler;
        {
            std::lock_guard<std::mutex> lock(eventHandlersMutex_);
            rejectionHandler = authenticationUnhandledRejectionHandler_;
        }
        std::optional<std::string> unhandledRejection;
        try {
            emitError(e.what());
        } catch (const std::exception& listenerError) {
            unhandledRejection = listenerError.what();
        } catch (...) {
            unhandledRejection = "Unknown asynchronous error";
        }
        if (unhandledRejection.has_value()) {
            dispatchAuthenticationUnhandledRejection(
                std::move(rejectionHandler),
                std::move(*unhandledRejection)
            );
        }
        return false;
    }

    if (rakNetStopRequested_.load()) {
        stopQueue();
        closed_.store(true);
        return false;
    }
    closed_.store(false);

    RakNetClientOptions rakOptions;
    rakOptions.host = options_.host;
    rakOptions.port = options_.port;
    rakOptions.mtu = options_.mtu;
    rakOptions.timeoutMs = options_.connectTimeoutMs;
    rakOptions.protocolVersion = session_.definition().protocolVersion() >= 589 ? 11 : 10;

    auto raknet = std::make_shared<RakNetClient>(rakOptions);
    raknet->setCallbackLifetimeProvider(callbackLifetimeProviderSnapshot());
    raknet->onConnected([this]() {
        RakNetCallbackScope callbackScope(*this);
        handleRakNetConnected();
    });
    raknet->onEncapsulated([this](const std::vector<uint8_t>& payload) {
        RakNetCallbackScope callbackScope(*this);
        dispatchRakNetPayload(payload);
    });
    raknet->onClose([this](const std::string& reason) {
        RakNetCallbackScope callbackScope(*this);
        // RakNetClient clears running/connected before this callback. For a
        // remote disconnect the worker-tail closes the still-allocated fd
        // after this callback returns. Client.close() calls connection.close()
        // again in JS, but that second transport close is observationally
        // idempotent; skip it here to avoid re-entering the transport callback.
        emitClose(reason, false, CloseOrigin::Transport);
    });

    std::function<void()> beforeTransportInstallTestHook;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        beforeTransportInstallTestHook = beforeTransportInstallTestHook_;
    }
    if (beforeTransportInstallTestHook) beforeTransportInstallTestHook();

    std::shared_ptr<RakNetClient> transport;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        // This lock orders installation against emitClose()'s transport move.
        // A close that already published its stop cannot be followed by a new
        // member transport after close() returns.
        if (!rakNetStopRequested_.load()) {
            raknet_ = std::move(raknet);
            transport = raknet_;
        }
    }
    if (!transport) {
        stopQueueIfRunning();
        closed_.store(true);
        return false;
    }

    std::function<void()> afterTransportInstallTestHook;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        afterTransportInstallTestHook = afterTransportInstallTestHook_;
    }
    if (afterTransportInstallTestHook) afterTransportInstallTestHook();

    if (rakNetStopRequested_.load()) {
        transport->requestStop();
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            if (raknet_ == transport) raknet_.reset();
        }
        stopQueue();
        closed_.store(true);
        return false;
    }

    if (!transport->connect()) {
        auto error = transport->error();
        stopQueue();
        closed_.store(true);
        if (rakNetStopRequested_.load()) {
            return false;
        }
        setStatus(BedrockNetworkClientStatus::Disconnected);
        emitError(error);
        return false;
    }

    std::function<void()> beforePhaseCommitTestHook;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        beforePhaseCommitTestHook = beforeConnectPhaseCommitTestHook_;
    }
    if (beforePhaseCommitTestHook) beforePhaseCommitTestHook();

    connectedSuccessfully = true;
    return true;
}

bool BedrockNetworkClient::connectPreparationOwnedByCurrentThread() {
    std::lock_guard<std::mutex> lock(connectLifecycleMutex_);
    return connectLifecyclePhase_ == ConnectLifecyclePhase::Preparing &&
        connectLifecycleOwnerThreadId_ == std::this_thread::get_id();
}

bool BedrockNetworkClient::prepareConnectLifecycle(bool deferQueuePump) {
    bool reentrantPreparation = false;
    for (;;) {
        std::unique_lock<std::mutex> lock(connectLifecycleMutex_);
        if (rakNetStopRequested_.load()) return false;
        if (connectLifecyclePhase_ == ConnectLifecyclePhase::Preparing) {
            if (connectLifecycleOwnerThreadId_ ==
                std::this_thread::get_id()) {
                // auth.authenticate() emits constructor failures before its
                // first await. EventEmitter therefore permits an error
                // listener to call client.connect() recursively. Run another
                // complete auth/startQueue pass without publishing over the
                // still-active outer preparation phase.
                reentrantPreparation = true;
                break;
            }
            connectLifecycleCv_.wait(lock, [this]() {
                return connectLifecyclePhase_ !=
                    ConnectLifecyclePhase::Preparing;
            });
            continue;
        }
        if (connectLifecyclePhase_ == ConnectLifecyclePhase::Prepared) {
            lock.unlock();
            if (!deferQueuePump) resumeQueuePump();
            return true;
        }
        if (connectLifecyclePhase_ == ConnectLifecyclePhase::Connecting ||
            connectLifecyclePhase_ == ConnectLifecyclePhase::Active) {
            return true;
        }
        connectLifecyclePhase_ = ConnectLifecyclePhase::Preparing;
        connectLifecycleOwnerThreadId_ = std::this_thread::get_id();
        break;
    }

    bool authenticationRejected = false;
    std::string authenticationError;
    std::optional<std::string> authenticationUnhandledRejection;
    AuthenticationUnhandledRejectionHandler rejectionHandler;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        // This is an internal process-boundary bridge, not an EventEmitter
        // listener. Retain the admitted callback even if an error listener
        // synchronously calls close()/removeAllListeners below.
        rejectionHandler = authenticationUnhandledRejectionHandler_;
    }
    bool queueStartedAfterTransportStop = false;
    try {
        // Client.connect() performs authentication setup before startQueue().
        // Offline auth validates only the username and does not run auth.js's
        // online option normalizer.
        if (options_.offline) {
            if (options_.username.empty()) {
                throw std::runtime_error("Must specify a valid username");
            }
        } else {
            auto authOptionsSnapshot = options();
            NativeBedrockAuthflowOptions normalizedAuthOptions {
                .username = authOptionsSnapshot.username,
                .profilesFolder = authOptionsSnapshot.profilesFolder,
                .authTitle = authOptionsSnapshot.authTitle,
                .deviceType = authOptionsSnapshot.deviceType,
                .flow = authOptionsSnapshot.flow,
                .forceRefresh = authOptionsSnapshot.forceRefresh,
                .msalConfig = authOptionsSnapshot.msalConfig,
                .password = authOptionsSnapshot.password,
                .onMsaCode = authOptionsSnapshot.onMsaCode
            };
            validateNativeBedrockAuthflowOptions(normalizedAuthOptions);
            authOptionsSnapshot.profilesFolder =
                normalizedAuthOptions.profilesFolder;
            authOptionsSnapshot.authTitle = normalizedAuthOptions.authTitle;
            authOptionsSnapshot.deviceType = normalizedAuthOptions.deviceType;
            authOptionsSnapshot.flow = normalizedAuthOptions.flow;

            // validateOptions() mutates the same public object synchronously,
            // before `new PrismarineAuth(...)` can validate flow or touch its
            // cache. Publish those mutations before reproducing constructor
            // work, while preserving every unrelated caller property.
            {
                std::lock_guard<std::mutex> lock(optionsMutex_);
                options_.authTitle = authOptionsSnapshot.authTitle;
                options_.deviceType = authOptionsSnapshot.deviceType;
                options_.flow = authOptionsSnapshot.flow;
                options_.profilesFolder = authOptionsSnapshot.profilesFolder;
                authOptionsSnapshot = options_;
            }

            AuthenticationOptionsResolvedHandler resolvedHandler;
            {
                std::lock_guard<std::mutex> lock(eventHandlersMutex_);
                resolvedHandler = authenticationOptionsResolvedHandler_;
            }
            if (resolvedHandler) resolvedHandler(authOptionsSnapshot);

            if (authOptionsSnapshot.authflow) {
                // `options.authflow || new PrismarineAuth(...)`: a truthy
                // supplied object bypasses the constructor, cache setup, flow
                // validation and msalConfig processing completely. Its method
                // call itself is synchronous; only the returned Promise is
                // deferred until after startQueue().
                try {
                    std::string clientX509;
                    {
                        std::lock_guard<std::mutex> lock(optionsMutex_);
                        if (clientKeys_.privateKeyPem.empty()) {
                            clientKeys_ =
                                BedrockAuthJwt::generateP384KeyPair();
                        }
                        clientX509 = clientKeys_.publicKeyDerBase64;
                    }
                    auto pending = authOptionsSnapshot.authflow->
                        getMinecraftBedrockToken(clientX509);
                    if (!pending.valid()) {
                        throw std::runtime_error(
                            "authflow.getMinecraftBedrockToken(...).catch "
                            "is not a function"
                        );
                    }
                    {
                        std::lock_guard<std::mutex> lock(optionsMutex_);
                        pendingAuthflowChains_ = std::move(pending);
                    }
                } catch (const std::exception& error) {
                    authenticationRejected = true;
                    authenticationError = error.what();
                }
            } else {
                NativeBedrockAuthflowOptions nativeAuthOptions {
                    .username = authOptionsSnapshot.username,
                    .profilesFolder = authOptionsSnapshot.profilesFolder,
                    .authTitle = authOptionsSnapshot.authTitle,
                    .deviceType = authOptionsSnapshot.deviceType,
                    .flow = authOptionsSnapshot.flow,
                    .forceRefresh = authOptionsSnapshot.forceRefresh,
                    .msalConfig = authOptionsSnapshot.msalConfig,
                    .password = authOptionsSnapshot.password,
                    .onMsaCode = authOptionsSnapshot.onMsaCode
                };

                try {
                    // MicrosoftAuthFlow checks flow before touching its cache.
                    validateNativeBedrockAuthflowPresence(nativeAuthOptions);
                    const auto effectiveCacheRoot =
                        initializeNativeBedrockAuthCacheRoot(
                            nativeAuthOptions.profilesFolder
                        );
                    {
                        std::lock_guard<std::mutex> lock(optionsMutex_);
                        authenticationCacheRoot_ = effectiveCacheRoot;
                        authenticationMsalConfig_.reset();
                        authenticationLiveTokenManager_.reset();
                        authenticationMsaTokenManager_.reset();
                        authenticationXboxTokenManager_.reset();
                        authenticationBedrockTokenManager_.reset();
                        authenticationBedrockServicesTokenManager_.reset();
                        authenticationPlayfabTokenManager_.reset();
                        authenticationMicrosoftAuthFlow_.reset();
                        authenticationXboxProofKey_.reset();
                        authenticationCaches_.clear();
                    }
                    auto authRuntime = createNativeBedrockAuthflow(
                        nativeAuthOptions,
                        effectiveCacheRoot
                    );
                    {
                        std::lock_guard<std::mutex> lock(optionsMutex_);
                        authenticationMsalConfig_ =
                            std::move(authRuntime.effectiveMsalConfig);
                        authenticationLiveTokenManager_ =
                            std::move(authRuntime.live);
                        authenticationMsaTokenManager_ =
                            std::move(authRuntime.msa);
                        authenticationXboxTokenManager_ =
                            std::move(authRuntime.xbox);
                        authenticationBedrockTokenManager_ =
                            std::move(authRuntime.bedrock);
                        authenticationBedrockServicesTokenManager_ =
                            std::move(authRuntime.bedrockServices);
                        authenticationPlayfabTokenManager_ =
                            std::move(authRuntime.playfab);
                        authenticationMicrosoftAuthFlow_ =
                            std::move(authRuntime.microsoft);
                        authenticationXboxProofKey_ =
                            std::move(authRuntime.xboxProofKey);
                        authenticationCaches_ =
                            std::move(authRuntime.caches);
                    }
                } catch (const std::exception& error) {
                    authenticationRejected = true;
                    authenticationError = error.what();
                }
            }

            if (authenticationRejected) {
                // authenticate() is async: even an unhandled error event or a
                // throwing listener rejects that ignored Promise rather than
                // aborting Client.connect(). Its synchronous listener snapshot
                // still runs here, before startQueue().
                try {
                    emitError(authenticationError);
                } catch (const std::exception& error) {
                    authenticationUnhandledRejection = error.what();
                } catch (...) {
                    authenticationUnhandledRejection =
                        "Unknown asynchronous error";
                }
            }
        }

        std::function<void()> beforeQueueStartTestHook;
        {
            std::lock_guard<std::mutex> lock(eventHandlersMutex_);
            beforeQueueStartTestHook = beforeQueueStartTestHook_;
        }
        if (beforeQueueStartTestHook) beforeQueueStartTestHook();
        queueStartedAfterTransportStop = authenticationRejected &&
            rakNetStopRequested_.load();
        if (!startQueue(
                !deferQueuePump || queueStartedAfterTransportStop,
                queueStartedAfterTransportStop
            )) {
            if (!reentrantPreparation) {
                {
                    std::lock_guard<std::mutex> lock(connectLifecycleMutex_);
                    authenticationRejected_ = false;
                    connectLifecyclePhase_ = ConnectLifecyclePhase::Idle;
                    connectLifecycleOwnerThreadId_ = std::thread::id{};
                }
                connectLifecycleCv_.notify_all();
            }
            return false;
        }
        resetLifecycle();
    } catch (...) {
        if (!reentrantPreparation) {
            {
                std::lock_guard<std::mutex> lock(connectLifecycleMutex_);
                authenticationRejected_ = false;
                connectLifecyclePhase_ = ConnectLifecyclePhase::Idle;
                connectLifecycleOwnerThreadId_ = std::thread::id{};
            }
            connectLifecycleCv_.notify_all();
        }
        throw;
    }

    if (reentrantPreparation) {
        // The nested JavaScript connect() has now performed its own
        // startQueue(). Keep the outer owner's phase and rejection result
        // untouched; it still has to run its later startQueue() and publish
        // Prepared. Promise rejection reporting, if any, belongs to this
        // nested authenticate() call and is independently deferred.
        if (authenticationUnhandledRejection.has_value()) {
            dispatchAuthenticationUnhandledRejection(
                std::move(rejectionHandler),
                std::move(*authenticationUnhandledRejection)
            );
        }
        return !rakNetStopRequested_.load();
    }

    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(connectLifecycleMutex_);
        stopped = rakNetStopRequested_.load();
        authenticationRejected_ = authenticationRejected;
        connectLifecyclePhase_ = stopped
            ? ConnectLifecyclePhase::Idle
            : ConnectLifecyclePhase::Prepared;
        connectLifecycleOwnerThreadId_ = std::thread::id{};
    }
    connectLifecycleCv_.notify_all();
    if (authenticationUnhandledRejection.has_value()) {
        dispatchAuthenticationUnhandledRejection(
            std::move(rejectionHandler),
            std::move(*authenticationUnhandledRejection)
        );
    }
    if (stopped) {
        // A full close may already have stopped the pump and returned before
        // this preparation owner observes the stop bit. Do not clear packets
        // queued by the caller after that close; only cancel a pump that is
        // still running (for example after requestRakNetStop()).
        if (!queueStartedAfterTransportStop) stopQueueIfRunning();
        return false;
    }
    return true;
}

int BedrockNetworkClient::run() {
    if (!connect()) {
        return 1;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    closedCv_.wait(lock, [this]() {
        return closed_.load();
    });
    return 0;
}

void BedrockNetworkClient::close(const std::string& reason) {
    emitClose(reason, true, CloseOrigin::Public);
}

void BedrockNetworkClient::beginDeferredClose(const std::string& reason) {
    emitClose(reason, false, CloseOrigin::Public);
}

void BedrockNetworkClient::disconnect(const std::string& reason, bool hide) {
    if (!sendDisconnectPacket(reason, hide)) return;
    close(reason);
}

bool BedrockNetworkClient::sendDisconnectPacket(
    const std::string& reason,
    bool hide
) {
    if (status() == BedrockNetworkClientStatus::Disconnected) {
        return false;
    }

    (void)hide;
    write("disconnect", ProtoDefValue::object({
        // Node ProtoDef supplies the mapper's first/default value when the JS
        // object omits this schema-only telemetry field.
        {"reason", ProtoDefValue::string("unknown")},
        // Match bedrock-protocol's observable field-name bug. client.js writes
        // hide_disconnect_screen, which ProtoDef ignores; the schema field
        // hide_disconnect_reason therefore remains false for either argument.
        {"hide_disconnect_reason", ProtoDefValue::boolean(false)},
        {"message", ProtoDefValue::string(reason)},
        {"filtered_message", ProtoDefValue::string("")}
    }));
    return true;
}

void BedrockNetworkClient::on(const std::string& packetName, PacketHandler handler) {
    if (packetName == "packet") {
        onAny(std::move(handler));
        return;
    }
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    namedHandlers_[packetName].push_back(std::move(handler));
}

void BedrockNetworkClient::onAny(PacketHandler handler) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    anyHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onJoin(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    joinHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onSpawn(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    spawnHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onHeartbeat(HeartbeatHandler handler) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    heartbeatHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onClose(ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    closeHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onClose(std::function<void()> handler) {
    onClose([handler = std::move(handler)](const std::string&) {
        handler();
    });
}

void BedrockNetworkClient::onError(ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    errorHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::onStatus(StatusHandler handler) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    statusHandlers_.push_back(std::move(handler));
}

void BedrockNetworkClient::setCallbackLifetimeProvider(
    CallbackLifetimeProvider provider
) {
    CallbackLifetimeProvider snapshot;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        callbackLifetimeProvider_ = std::move(provider);
        snapshot = callbackLifetimeProvider_;
    }
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (raknet_) {
        raknet_->setCallbackLifetimeProvider(std::move(snapshot));
    }
}

void BedrockNetworkClient::setAuthenticationOptionsResolvedHandler(
    AuthenticationOptionsResolvedHandler handler
) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    authenticationOptionsResolvedHandler_ = std::move(handler);
}

void BedrockNetworkClient::setAuthenticationUnhandledRejectionHandler(
    AuthenticationUnhandledRejectionHandler handler
) {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    authenticationUnhandledRejectionHandler_ = std::move(handler);
}

bool BedrockNetworkClient::onRakNetCallbackThread() const {
    std::lock_guard<std::mutex> lock(rakNetCallbackMutex_);
    return rakNetCallbackDepth_ != 0 &&
        rakNetCallbackThreadId_ == std::this_thread::get_id();
}

bool BedrockNetworkClient::onCloseEmissionThread() const {
    std::lock_guard<std::mutex> lock(closingMutex_);
    return closing_.load() &&
        closingThreadId_ == std::this_thread::get_id();
}

void BedrockNetworkClient::requestRakNetStop() noexcept {
    rakNetStopRequested_.store(true);
    std::shared_ptr<RakNetClient> transport;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        transport = raknet_;
    }
    if (transport) transport->requestStop();
}

void BedrockNetworkClient::enterRakNetCallback() {
    std::lock_guard<std::mutex> lock(rakNetCallbackMutex_);
    const auto current = std::this_thread::get_id();
    if (rakNetCallbackDepth_ == 0) {
        rakNetCallbackThreadId_ = current;
    } else if (rakNetCallbackThreadId_ != current) {
        throw std::logic_error("concurrent RakNet callback scopes");
    }
    ++rakNetCallbackDepth_;
}

void BedrockNetworkClient::leaveRakNetCallback() noexcept {
    std::lock_guard<std::mutex> lock(rakNetCallbackMutex_);
    if (rakNetCallbackDepth_ == 0 ||
        rakNetCallbackThreadId_ != std::this_thread::get_id()) {
        return;
    }
    --rakNetCallbackDepth_;
    if (rakNetCallbackDepth_ == 0) {
        rakNetCallbackThreadId_ = std::thread::id{};
    }
}

BedrockNetworkClient::CallbackLifetimeProvider
BedrockNetworkClient::callbackLifetimeProviderSnapshot() const {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    return callbackLifetimeProvider_;
}

void BedrockNetworkClient::sendPacket(const VersionedGamePacket& packet) {
    sendPackets({packet});
}

void BedrockNetworkClient::sendBuffer(const std::vector<uint8_t>& buffer, bool immediate) {
    auto packet = session_.packetCodec().decodeFullPacket(buffer);
    if (packet.name == "start_game" || packet.name == "item_registry") {
        (void) packetDecoder_.decodePacket(packet.name, packet.payload);
    }
    if (immediate) {
        sendPacket(packet);
        return;
    }

    std::lock_guard<std::mutex> lock(queueMutex_);
    queuedPackets_.push_back(std::move(packet));
}

void BedrockNetworkClient::send(const std::string& packetName, const ProtoDefValue& value) {
    auto payload = packetEncoder_.encodePacket(packetName, value);
    sendPacket(session_.packetCodec().makePacketByName(packetName, payload));
}

void BedrockNetworkClient::write(const std::string& packetName, const ProtoDefValue& value) {
    send(packetName, value);
}

void BedrockNetworkClient::queue(const std::string& packetName, const ProtoDefValue& value) {
    auto payload = packetEncoder_.encodePacket(packetName, value);
    std::lock_guard<std::mutex> lock(queueMutex_);
    queuedPackets_.push_back(session_.packetCodec().makePacketByName(packetName, payload));
}

void BedrockNetworkClient::sendQueued() {
    std::vector<VersionedGamePacket> packets;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        packets = std::move(queuedPackets_);
        queuedPackets_.clear();
    }
    sendPackets(packets);
}

BedrockNetworkClientStatus BedrockNetworkClient::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::optional<uint64_t> BedrockNetworkClient::entityId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return runtimeEntityId_;
}

uint32_t BedrockNetworkClient::protocolVersion() const {
    return options_.protocolVersion;
}

bool BedrockNetworkClient::versionLessThan(const std::string& version) const {
    const auto target = connectionComparisonProtocolVersion(version);
    return target && versionLessThan(*target);
}

bool BedrockNetworkClient::versionLessThan(uint32_t protocolVersion) const noexcept {
    return options_.protocolVersion < protocolVersion;
}

bool BedrockNetworkClient::versionGreaterThan(const std::string& version) const {
    const auto target = connectionComparisonProtocolVersion(version);
    return target && versionGreaterThan(*target);
}

bool BedrockNetworkClient::versionGreaterThan(uint32_t protocolVersion) const noexcept {
    return options_.protocolVersion > protocolVersion;
}

bool BedrockNetworkClient::versionGreaterThanOrEqualTo(const std::string& version) const {
    const auto target = connectionComparisonProtocolVersion(version);
    return target && versionGreaterThanOrEqualTo(*target);
}

bool BedrockNetworkClient::versionGreaterThanOrEqualTo(uint32_t protocolVersion) const noexcept {
    return options_.protocolVersion >= protocolVersion;
}

bool BedrockNetworkClient::versionLessThanOrEqualTo(const std::string& version) const {
    const auto target = connectionComparisonProtocolVersion(version);
    return target && versionLessThanOrEqualTo(*target);
}

bool BedrockNetworkClient::versionLessThanOrEqualTo(uint32_t protocolVersion) const noexcept {
    return options_.protocolVersion <= protocolVersion;
}

BedrockNetworkClientOptions BedrockNetworkClient::options() const {
    std::lock_guard<std::mutex> lock(optionsMutex_);
    return options_;
}

const VersionedClientSession& BedrockNetworkClient::session() const {
    return session_;
}

VersionedClientSession& BedrockNetworkClient::session() {
    return session_;
}

const BedrockWorld& BedrockNetworkClient::world() const {
    return world_;
}

BedrockWorld& BedrockNetworkClient::world() {
    return world_;
}

const BedrockBlobStore& BedrockNetworkClient::blobStore() const {
    return blobStore_;
}

BedrockBlobStore& BedrockNetworkClient::blobStore() {
    return blobStore_;
}

ProtoDefVariableStorePtr BedrockNetworkClient::packetVariableStore() const {
    return packetVariables_;
}

BedrockNetworkClientOptions BedrockNetworkClient::normalizeOptions(BedrockNetworkClientOptions options) {
    options.protocolVersion = validateVersion(options.version);
    if (options.profile.empty() && !options.username.empty()) {
        options.profile = options.username;
    }

    if (options.connectTimeout.has_value()) {
        const int value = *options.connectTimeout;
        options.connectTimeoutMs = value == 0 ? 9000 : std::max(value, 1);
    }
    if (options.batchingInterval.has_value()) {
        const int value = *options.batchingInterval;
        options.batchingIntervalMs = value == 0 ? 20 : std::max(value, 1);
    }
    if (options.useNativeRaknet.has_value()) {
        options.raknetBackend = *options.useNativeRaknet
            ? "raknet-native"
            : "jsp-raknet";
    }
    if (!options.raknetBackend.empty() &&
        options.raknetBackend != "raknet-native") {
        throw std::runtime_error(
            "RakNet backend is not implemented in C++: " +
            options.raknetBackend
        );
    }
    return options;
}

bool BedrockNetworkClient::versionAtLeast(const std::string& version, int major, int minor, int patch) {
    std::vector<int> parts;
    std::string current;
    for (char c : version) {
        if (c == '.') {
            parts.push_back(current.empty() ? 0 : std::stoi(current));
            current.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            current.push_back(c);
        }
    }
    parts.push_back(current.empty() ? 0 : std::stoi(current));
    while (parts.size() < 3) parts.push_back(0);

    if (parts[0] != major) return parts[0] > major;
    if (parts[1] != minor) return parts[1] > minor;
    return parts[2] >= patch;
}

bool BedrockNetworkClient::versionAtMost(
    const std::string& version,
    const std::string& maximum
) {
    return ProtocolDefinition::forVersion(version).protocolVersion() <=
        ProtocolDefinition::forVersion(maximum).protocolVersion();
}

void BedrockNetworkClient::setStatus(BedrockNetworkClientStatus status) {
    // Match Connection.status in bedrock-protocol: listeners observe the old
    // value while handling the status event, then the new value is stored.
    const auto provider = callbackLifetimeProviderSnapshot();
    auto lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    if (!provider || lifetimeLease) {
        std::vector<StatusHandler> handlers;
        {
            std::lock_guard<std::mutex> lock(eventHandlersMutex_);
            handlers = statusHandlers_;
        }
        for (auto& handler : handlers) {
            handler(status);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A callback may synchronously destroy/close the factory facade. Its
        // terminal cleanup wins over this outer status setter after the full
        // listener snapshot has completed; otherwise a native callback frame
        // would resurrect a closed network after its last owner is gone.
        if (rakNetStopRequested_.load() &&
            status_ == BedrockNetworkClientStatus::Disconnected &&
            status != BedrockNetworkClientStatus::Disconnected) {
            return;
        }
        status_ = status;
    }
}

void BedrockNetworkClient::emitError(const std::string& message) {
    const auto provider = callbackLifetimeProviderSnapshot();
    auto lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    if (provider && !lifetimeLease) {
        return;
    }
    std::vector<ErrorHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        handlers = errorHandlers_;
    }
    if (handlers.empty()) {
        // EventEmitter gives `error` special treatment: without a listener the
        // Error itself is thrown, so the caller does not continue to its next
        // statement (notably encryption.js's bad-packet disconnect).
        throw BedrockNetworkClientUnhandledError(message);
    }

    // EventEmitter snapshots an array of listeners before invoking it. A
    // listener may close the client and clear the member storage; the snapshot
    // must still be safe and remaining listeners must retain their JS order.
    for (auto& handler : handlers) {
        handler(message);
    }
}

void BedrockNetworkClient::emitClose(
    const std::string& reason,
    bool closeTransport,
    CloseOrigin origin
) {
    {
        std::unique_lock<std::mutex> lock(closingMutex_);
        const auto current = std::this_thread::get_id();
        if (origin == CloseOrigin::Transport && closing_.load()) {
            if (closingThreadId_ == current) {
                // Synchronous callback caused by this public close is the same
                // native teardown, not a second JavaScript close.
                return;
            }
            // A remote Rak worker cannot wait through the later transport
            // join. Successful public listener completion publishes commit
            // first, allowing this callback to unwind before that join. If a
            // listener throws, the latch opens without commit and this remote
            // close becomes the cleanup owner.
            closingCv_.wait(lock, [this]() {
                return !closing_.load() || closingCommitted_;
            });
            if (closingCommitted_) return;
        }
        if (closing_.load() && closingThreadId_ == current) {
            // Public recursive close is legal EventEmitter recursion. Keep a
            // depth count so the inner cleanup cannot release the outer
            // serialization latch or invalidate its listener snapshot.
            ++closingDepth_;
        } else {
            closingCv_.wait(lock, [this]() { return !closing_.load(); });
            closing_.store(true);
            closingThreadId_ = current;
            closingDepth_ = 1;
            closingCommitted_ = false;
        }
    }

    // This lease must outlive closingReset. A factory listener may destroy
    // its Client through either Client::close() or client.network().close();
    // reverse local destruction must release the close latch before the final
    // State/network owner can disappear.
    std::shared_ptr<void> lifetimeLease;
    pauseQueuePump();
    bool closeCommitted = false;
    auto closingReset = networkScopeExit(
        [this, &closeCommitted]() {
            bool notify = false;
            bool notifyQueue = false;
            {
                std::lock_guard<std::mutex> lock(closingMutex_);
                if (closingDepth_ > 0) --closingDepth_;
                if (closingDepth_ == 0) {
                    // A failed recursive frame must not re-arm the pump while
                    // its outer EventEmitter snapshot is still running. The
                    // outermost frame alone owns the pre-emission pump state;
                    // also preserve a factory queue that was intentionally
                    // paused through connect_allowed.
                    // Publish the rollback timer and the open close latch as
                    // one queue-visible transition. resume/start either run
                    // before admission and are caught by pauseQueuePump(), or
                    // run after admission and leave only logical demand. They
                    // cannot enable a native tick inside the listener stack.
                    std::lock_guard<std::mutex> queueLock(queueMutex_);
                    if (!closeCommitted && !closingCommitted_) {
                        const auto interval = std::chrono::milliseconds(
                            options_.batchingIntervalMs > 0
                                ? options_.batchingIntervalMs
                                : 20
                        );
                        queuePumpEnabled_ = false;
                        queuePumpResumeDue_ =
                            std::chrono::steady_clock::now() + interval;
                        notifyQueue = true;
                    }
                    closingThreadId_ = std::thread::id{};
                    closing_.store(false);
                    closingCommitted_ = false;
                    notify = true;
                }
            }
            if (notifyQueue) queueCv_.notify_all();
            if (notify) closingCv_.notify_all();
        }
    );

    const auto provider = callbackLifetimeProviderSnapshot();
    lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    const bool callbacksAdmitted = !provider || lifetimeLease;

    // Client.close emits first. The callback intentionally observes the old
    // status and a still-live queue/transport, just like EventEmitter.emit.
    if (callbacksAdmitted &&
        status() != BedrockNetworkClientStatus::Disconnected) {
        std::vector<ErrorHandler> handlers;
        {
            std::lock_guard<std::mutex> lock(eventHandlersMutex_);
            handlers = closeHandlers_;
        }
        for (auto& handler : handlers) {
            handler(reason);
        }
    }

    // In JavaScript, a throwing close listener aborts close() before the
    // queue/timer and transport cleanup. Keep the native pump paused on that
    // exceptional path: immediately resuming it from this stack would let its
    // independent thread run before the synchronous caller receives/catches
    // the exception, unlike a Node timer. The queue loop re-arms itself at the
    // next native interval, after preserving the immediate catch boundary; a
    // later close() can also retry the emission and complete cleanup.
    rakNetStopRequested_.store(true);
    stopQueue();
    closeCommitted = true;
    {
        std::lock_guard<std::mutex> lock(closingMutex_);
        closingCommitted_ = true;
    }
    closingCv_.notify_all();

    std::shared_ptr<RakNetClient> ownedRakNet;
    RakNetClient* callbackThreadRakNet = nullptr;
    if (closeTransport) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (raknet_ && onRakNetCallbackThread()) {
            // Do not destroy RakNetClient while its own callback stack is
            // active. Its stopped instance remains owned until destruction or
            // a later reconnect.
            callbackThreadRakNet = raknet_.get();
        } else {
            ownedRakNet = std::move(raknet_);
        }
    }
    if (callbackThreadRakNet) {
        callbackThreadRakNet->close(reason);
    } else if (ownedRakNet) {
        ownedRakNet->close(reason);
    }

    // removeAllListeners precedes the final status assignment in client.js;
    // consequently no previously registered status listener sees
    // Disconnected.
    clearEventHandlers();
    setStatus(BedrockNetworkClientStatus::Disconnected);
    closed_.store(true);
    {
        std::lock_guard<std::mutex> lock(connectLifecycleMutex_);
        if (connectLifecyclePhase_ != ConnectLifecyclePhase::Connecting) {
            connectLifecyclePhase_ = ConnectLifecyclePhase::Idle;
        }
    }
    connectLifecycleCv_.notify_all();
    closedCv_.notify_all();
}

void BedrockNetworkClient::clearEventHandlers() {
    std::lock_guard<std::mutex> lock(eventHandlersMutex_);
    anyHandlers_.clear();
    namedHandlers_.clear();
    joinHandlers_.clear();
    spawnHandlers_.clear();
    heartbeatHandlers_.clear();
    closeHandlers_.clear();
    errorHandlers_.clear();
    statusHandlers_.clear();
}

void BedrockNetworkClient::emitAnyPacket(const VersionedGamePacket& packet) {
    const auto provider = callbackLifetimeProviderSnapshot();
    auto lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    if (provider && !lifetimeLease) return;
    BedrockNetworkClientPacketEvent event;
    event.packet = packet;

    std::vector<PacketHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        handlers = anyHandlers_;
    }
    for (auto& handler : handlers) {
        handler(event);
    }
}

void BedrockNetworkClient::emitNamedPacket(const VersionedGamePacket& packet) {
    emitNamedEvent(packet.name, packet);
}

void BedrockNetworkClient::emitNamedEvent(
    const std::string& eventName,
    const VersionedGamePacket& packet
) {
    const auto provider = callbackLifetimeProviderSnapshot();
    auto lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    if (provider && !lifetimeLease) return;
    BedrockNetworkClientPacketEvent event;
    event.packet = packet;

    std::vector<PacketHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        auto it = namedHandlers_.find(eventName);
        if (it != namedHandlers_.end()) handlers = it->second;
    }
    if (!handlers.empty()) {
        // EventEmitter snapshots its listeners for an emission.  A special
        // event handler may close the client (and clear namedHandlers_), so do
        // the same here instead of iterating storage that can be invalidated.
        for (auto& handler : handlers) {
            handler(event);
        }
    }
}

void BedrockNetworkClient::emitJoin() {
    const auto provider = callbackLifetimeProviderSnapshot();
    auto lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    if (provider && !lifetimeLease) return;
    std::vector<std::function<void()>> handlers;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        handlers = joinHandlers_;
    }
    for (auto& handler : handlers) {
        handler();
    }
}

void BedrockNetworkClient::emitSpawn() {
    const auto provider = callbackLifetimeProviderSnapshot();
    auto lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    if (provider && !lifetimeLease) return;
    std::vector<std::function<void()>> handlers;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        handlers = spawnHandlers_;
    }
    for (auto& handler : handlers) {
        handler();
    }
}

void BedrockNetworkClient::emitHeartbeat(int64_t tick) {
    const auto provider = callbackLifetimeProviderSnapshot();
    auto lifetimeLease = provider
        ? provider()
        : std::shared_ptr<void>();
    if (provider && !lifetimeLease) return;
    std::vector<HeartbeatHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(eventHandlersMutex_);
        handlers = heartbeatHandlers_;
    }
    for (auto& handler : handlers) {
        handler(tick);
    }
}

void BedrockNetworkClient::handleRakNetConnected() {
    setStatus(BedrockNetworkClientStatus::Connecting);

    if (versionAtLeast(options_.version, 1, 19, 30) && session_.definition().hasPacket("request_network_settings")) {
        auto request = session_.writeNetworkSettingsRequest(session_.definition().protocolVersion());
        sendPackets({request});
        session_.takeOutgoingPackets();
        return;
    }

    sendLogin();
}

void BedrockNetworkClient::dispatchRakNetPayload(
    const std::vector<uint8_t>& payload
) {
    // Connection.handle/Framer, encryption transforms and user listeners run
    // inside raknet-native's live EventEmitter callback. Native packet parse
    // failures are retained by RakNetClient, but exceptions from this live
    // callback deliberately cross that boundary (the JS wrapper's unhandled
    // low-level `error` event does the same); they are not Client `error`s.
    handleRakNetPayload(payload);
}

void BedrockNetworkClient::handleRakNetPayload(const std::vector<uint8_t>& payload) {
    if (payload.empty() || payload[0] != 0xfe) {
        return;
    }

    VersionedMcpePayload decoded;
    if (encryptionEnabled_) {
        if (!decryptStream_) {
            throw std::runtime_error("decrypt stream is not initialized");
        }

        if (payload[0] != 0xfe) {
            throw std::runtime_error("encrypted MCPE payload missing 0xfe header");
        }

        std::vector<uint8_t> encryptedOnly(payload.begin() + 1, payload.end());
        auto verification = BedrockEncryption::decryptAndVerify(
            *decryptStream_,
            encryptedOnly,
            receiveCounter_,
            encryptionKeys_.secretKeyBytes
        );
        if (!verification) {
            return;
        }

        if (!verification->matches()) {
            const auto message = verification->mismatchMessage();
            emitError(message);
            disconnect("disconnectionScreen.badPacket");
            return;
        }

        const auto& compressionPacket = verification->packetPlaintext;
        if (session_.mcpeCodec().compressorInPacketHeader() &&
            (compressionPacket.empty() ||
             (compressionPacket[0] != 0x00 && compressionPacket[0] != 0xff))) {
            const auto compressor = compressionPacket.empty()
                ? std::string("undefined")
                : std::to_string(compressionPacket[0]);
            emitError("Unsupported compressor: " + compressor);

            // JS continues with an undefined buffer after a handled error and
            // then throws in onDecryptedPacket. C++ cannot represent undefined;
            // use a transport-boundary failure with the same no-disconnect
            // behavior after the exact error event.
            throw std::runtime_error(
                "unsupported compressor produced no decrypted buffer"
            );
        }

        decoded = session_.mcpeCodec().decodeEncryptedCompressionPacket(
            compressionPacket
        );
    } else {
        const auto& codec = session_.mcpeCodec();
        if (codec.compressorInPacketHeader() && !compressionReady_) {
            decoded = codec.decodeUncompressedMcpePayload(payload);
        } else if (!codec.compressorInPacketHeader() &&
                   compressionReady_ &&
                   compressionAlgorithm_ == "snappy") {
            decoded = codec.decodeMcpePayload(payload, "snappy");
        } else {
            // Preserve the original deflate-or-raw probe for the default,
            // pre-negotiation and unknown-algorithm legacy paths.
            decoded = codec.decodeMcpePayload(payload);
        }
    }

    for (const auto& packet : decoded.batch.packets) {
        handlePacket(packet);
    }

    drainSessionOutgoing();
}

void BedrockNetworkClient::handlePacket(const VersionedGamePacket& packet) {
    std::vector<ProtoDefField> decodedFields;
    std::string serverHandshakeToken;
    try {
        // Client.readPacket catches only deserializer.parsePacketBuffer. Keep
        // the C++ parsers used by built-in client handling in this narrow
        // pre-dispatch region; framing, decompression, internal handlers, and
        // user event callbacks remain uncaught at the transport boundary.
        if (packet.name == "network_settings" ||
            packet.name == "server_to_client_handshake" ||
            packet.name == "start_game" ||
            packet.name == "item_registry") {
            decodedFields = packetDecoder_.decodePacket(packet.name, packet.payload);
            if (packet.name == "server_to_client_handshake") {
                serverHandshakeToken = findFieldValue(decodedFields, "token");
            }
        }
        if (packet.name == "start_game") {
            (void) VersionedPayloadReader::readStartGame(packet);
        } else if (packet.name == "play_status") {
            (void) VersionedPayloadReader::readPlayStatus(packet);
        } else if (packet.name == "tick_sync") {
            (void) readI64LE(packet.payload, 0);
            (void) readI64LE(packet.payload, 8);
        }
    } catch (const std::exception& error) {
        emitError(error.what());
        return;
    }

    emitAnyPacket(packet);

    if (packet.name == "network_settings") {
        {
            std::lock_guard<std::mutex> lock(sendMutex_);
            compressionThreshold_ = parseU16Field(
                decodedFields,
                "compression_threshold",
                compressionThreshold_
            );
            auto algorithm = findFieldValue(decodedFields, "compression_algorithm");
            const auto mappedSeparator = algorithm.find('/');
            if (mappedSeparator != std::string::npos) {
                algorithm.erase(0, mappedSeparator + 1);
            }
            compressionAlgorithm_ = algorithm.empty() ? "deflate" : algorithm;
            compressionReady_ = true;
        }

        if (status() == BedrockNetworkClientStatus::Connecting) {
            sendLogin();
        }
        emitNamedPacket(packet);
        return;
    }

    if (packet.name == "server_to_client_handshake") {
        startEncryptionFromServerHandshake(serverHandshakeToken);
        emitNamedPacket(packet);
        return;
    }

    if (packet.name == "disconnect") {
        emitNamedPacket(packet);
        // client.js emits the protocol-level disconnect first, then the
        // documented special `kick` event with those same decoded params.
        // This is a named-only emission: the earlier `packet` event must not
        // be repeated.
        emitNamedEvent("kick", packet);
        close("Server requested disconnect");
        return;
    }

    if (packet.name == "level_chunk" && options_.trackWorld) {
        handleLevelChunk(packet);
    }

    if (packet.name == "subchunk" && options_.trackWorld) {
        handleSubChunk(packet);
    }

    if (packet.name == "client_cache_miss_response" && options_.trackWorld) {
        handleClientCacheMissResponse(packet);
    }

    session_.handlePacket(packet);

    if (packet.name == "resource_packs_info") handleResourcePacksInfo();
    else if (packet.name == "resource_pack_stack") handleResourcePackStack();
    else if (packet.name == "start_game") handleStartGame(packet);
    else if (packet.name == "play_status") handlePlayStatus(packet);
    else if (packet.name == "tick_sync") handleTickSync(packet);

    emitNamedPacket(packet);
}

void BedrockNetworkClient::handleResourcePacksInfo() {
    if (!options_.autoResourcePackResponses || resourcePacksInfoHandled_) {
        return;
    }

    resourcePacksInfoHandled_ = true;
    resourcePackStackHandlerArmed_ = true;

    write("resource_pack_client_response", ProtoDefValue::object({
        {"response_status", ProtoDefValue::string("completed")},
        {"resourcepackids", ProtoDefValue::array({})}
    }));

    queue("client_cache_status", ProtoDefValue::object({
        {"enabled", ProtoDefValue::boolean(false)}
    }));

    if (versionAtMost(options_.version, "1.20.80") &&
        session_.definition().hasPacket("tick_sync")) {
        queue("tick_sync", ProtoDefValue::object({
            {"request_time", ProtoDefValue::integer(unixTimeMillis())},
            {"response_time", ProtoDefValue::integer(0)}
        }));
    }

    std::lock_guard<std::mutex> lock(queueMutex_);
    chunkRadiusDue_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
}

void BedrockNetworkClient::handleResourcePackStack() {
    if (!options_.autoResourcePackResponses || !resourcePackStackHandlerArmed_) {
        return;
    }

    resourcePackStackHandlerArmed_ = false;
    write("resource_pack_client_response", ProtoDefValue::object({
        {"response_status", ProtoDefValue::string("completed")},
        {"resourcepackids", ProtoDefValue::array({})}
    }));
}

void BedrockNetworkClient::handleStartGame(const VersionedGamePacket& packet) {
    const auto startGame = VersionedPayloadReader::readStartGame(packet);
    bool sendInitialized = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimeEntityId_ = startGame.runtimeEntityId;
        // JS installs a persistent `on("start_game")` handler for this race,
        // so every later start_game writes the initialization packet.
        sendInitialized = initializeOnNextStartGame_;
    }

    if (sendInitialized) {
        sendLocalPlayerInitialized(startGame.runtimeEntityId);
    }
}

void BedrockNetworkClient::handlePlayStatus(const VersionedGamePacket& packet) {
    const int32_t code = playStatusCode(packet);
    if (code == 0 && status() == BedrockNetworkClientStatus::Authenticating) {
        emitJoin();
        setStatus(BedrockNetworkClientStatus::Initializing);
    }

    if (code != 3 || status() != BedrockNetworkClientStatus::Initializing ||
        !options_.autoInitPlayer) {
        return;
    }

    setStatus(BedrockNetworkClientStatus::Initialized);

    std::optional<uint64_t> runtimeId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (runtimeEntityId_.has_value() && *runtimeEntityId_ != 0) {
            runtimeId = runtimeEntityId_;
        } else {
            initializeOnNextStartGame_ = true;
        }
    }

    if (runtimeId.has_value()) {
        sendLocalPlayerInitialized(*runtimeId);
    }

    if (versionAtMost(options_.version, "1.20.80") &&
        session_.definition().hasPacket("tick_sync")) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        tick_ = 0;
        keepAliveDue_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    }

    emitSpawn();
}

void BedrockNetworkClient::handleTickSync(const VersionedGamePacket& packet) {
    bool heartbeatArmed = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        heartbeatArmed = keepAliveDue_.has_value();
    }
    if (!heartbeatArmed) {
        return;
    }

    const int64_t responseTime = readI64LE(packet.payload, 8);
    emitHeartbeat(responseTime);

    // Match JS ordering: heartbeat listeners observe the old tick value, then
    // response_time becomes the next keepalive request_time.
    std::lock_guard<std::mutex> lock(queueMutex_);
    tick_ = responseTime;
}

void BedrockNetworkClient::handleLevelChunk(const VersionedGamePacket& packet) {
    try {
        auto levelChunk = BedrockLevelChunkCodec::decodePacketPayload(packet.payload);
        if (!tryStoreLevelChunk(levelChunk)) {
            pendingCachedLevelChunks_.push_back(std::move(levelChunk));
        }
    } catch (const std::exception& e) {
        emitError("level_chunk decode failed: " + std::string(e.what()));
    }
}

void BedrockNetworkClient::handleSubChunk(const VersionedGamePacket& packet) {
    try {
        const auto subChunkPacket = BedrockSubChunkPacketCodec::decodePacketPayload(
            packet.payload,
            options_.version
        );

        BedrockClientCacheBlobStatus cacheStatus;
        for (const auto& entry : subChunkPacket.entries) {
            const int32_t chunkX = subChunkPacket.originX + entry.dx;
            const int32_t sectionY = subChunkPacket.originY + entry.dy;
            const int32_t chunkZ = subChunkPacket.originZ + entry.dz;

            if (entry.result == BedrockSubChunkResult::SuccessAllAir) {
                ensureTrackedColumn(chunkX, chunkZ).setSection(
                    sectionY,
                    BedrockSubChunk::createAir(static_cast<int8_t>(sectionY), 0)
                );
                continue;
            }
            if (entry.result != BedrockSubChunkResult::Success) {
                continue;
            }

            if (!subChunkPacket.cacheEnabled) {
                ensureTrackedColumn(chunkX, chunkZ).networkDecodeSubChunkNoCache(
                    sectionY,
                    entry.payload
                );
                continue;
            }
            if (!entry.blobId.has_value()) {
                throw BedrockChunkError("cached subchunk response is missing blob id");
            }

            PendingBlobMetadata metadata;
            metadata.type = BlobType::ChunkSection;
            metadata.x = chunkX;
            metadata.y = sectionY;
            metadata.z = chunkZ;
            pendingBlobMetadata_[*entry.blobId] = metadata;

            PendingCachedSubChunk pending;
            pending.chunkX = chunkX;
            pending.sectionY = sectionY;
            pending.chunkZ = chunkZ;
            pending.hash = *entry.blobId;
            pending.blockEntityPayload = entry.payload;
            if (tryStoreCachedSubChunk(pending)) {
                cacheStatus.have.push_back(pending.hash);
            } else {
                cacheStatus.missing.push_back(pending.hash);
                pendingCachedSubChunks_.push_back(std::move(pending));
            }
        }

        if (!cacheStatus.have.empty() || !cacheStatus.missing.empty()) {
            const auto payload = BedrockLevelChunkCodec::encodeClientCacheBlobStatusPayload(
                cacheStatus
            );
            sendPacket(session_.packetCodec().makePacketByName(
                "client_cache_blob_status",
                payload
            ));
        }
    } catch (const std::exception& error) {
        emitError("subchunk decode failed: " + std::string(error.what()));
    }
}

void BedrockNetworkClient::handleClientCacheMissResponse(const VersionedGamePacket& packet) {
    try {
        auto blobs = BedrockLevelChunkCodec::decodeClientCacheMissResponsePayload(packet.payload);
        for (auto& blob : blobs) {
            BlobEntry entry;
            entry.type = BlobType::Biomes;
            auto metadataIt = pendingBlobMetadata_.find(blob.hash);
            if (metadataIt != pendingBlobMetadata_.end()) {
                entry.type = metadataIt->second.type;
                entry.x = metadataIt->second.x;
                entry.y = metadataIt->second.y;
                entry.z = metadataIt->second.z;
            }
            entry.buffer = std::move(blob.payload);
            blobStore_.set(blob.hash, std::move(entry));
            pendingBlobMetadata_.erase(blob.hash);
        }

        std::vector<BedrockLevelChunkPacket> stillPending;
        for (const auto& levelChunk : pendingCachedLevelChunks_) {
            if (!tryStoreLevelChunk(levelChunk)) {
                stillPending.push_back(levelChunk);
            }
        }
        pendingCachedLevelChunks_ = std::move(stillPending);

        std::vector<PendingCachedSubChunk> stillPendingSubChunks;
        for (const auto& subChunk : pendingCachedSubChunks_) {
            if (!tryStoreCachedSubChunk(subChunk)) {
                stillPendingSubChunks.push_back(subChunk);
            }
        }
        pendingCachedSubChunks_ = std::move(stillPendingSubChunks);
    } catch (const std::exception& e) {
        emitError("client_cache_miss_response decode failed: " + std::string(e.what()));
    }
}

bool BedrockNetworkClient::tryStoreLevelChunk(const BedrockLevelChunkPacket& levelChunk) {
    if (!levelChunk.cacheEnabled) {
        auto column = BedrockLevelChunkCodec::decodeNoCacheColumn(
            levelChunk,
            versionAtLeast(options_.version, 1, 18, 0)
        );
        world_.setLoadedColumn(levelChunk.x, levelChunk.z, std::move(column));
        return true;
    }

    BedrockClientCacheBlobStatus status;
    const std::size_t legacySectionCount =
        levelChunk.subChunkCount > 0 && levelChunk.blobHashes.size() > 1
        ? std::min<std::size_t>(
            static_cast<std::size_t>(levelChunk.subChunkCount),
            levelChunk.blobHashes.size() - 1
        )
        : 0;
    for (std::size_t i = 0; i < levelChunk.blobHashes.size(); ++i) {
        const auto hash = levelChunk.blobHashes[i];
        PendingBlobMetadata metadata;
        metadata.type = i < legacySectionCount
            ? BlobType::ChunkSection
            : BlobType::Biomes;
        metadata.x = levelChunk.x;
        metadata.y = i < legacySectionCount ? static_cast<int32_t>(i) : 0;
        metadata.z = levelChunk.z;
        pendingBlobMetadata_[hash] = metadata;
        if (blobStore_.has(hash)) {
            status.have.push_back(hash);
        } else {
            status.missing.push_back(hash);
        }
    }

    if (!levelChunk.blobHashes.empty()) {
        auto payload = BedrockLevelChunkCodec::encodeClientCacheBlobStatusPayload(status);
        sendPacket(session_.packetCodec().makePacketByName("client_cache_blob_status", payload));
    }

    BedrockChunkColumn column(levelChunk.x, levelChunk.z);
    if (versionAtLeast(options_.version, 1, 18, 0)) {
        column.setBounds(-4, 20);
    }
    auto misses = column.networkDecodeCached(levelChunk.blobHashes, blobStore_, levelChunk.payload);
    if (!misses.empty()) {
        return false;
    }
    world_.setLoadedColumn(levelChunk.x, levelChunk.z, std::move(column));
    return true;
}

bool BedrockNetworkClient::tryStoreCachedSubChunk(
    const PendingCachedSubChunk& subChunk
) {
    const BlobEntry* blob = blobStore_.get(subChunk.hash);
    if (blob == nullptr) {
        return false;
    }
    if (blob->type != BlobType::ChunkSection) {
        throw BedrockChunkError("cached subchunk blob has an unexpected type");
    }

    std::vector<uint8_t> payload = blob->buffer;
    payload.insert(
        payload.end(),
        subChunk.blockEntityPayload.begin(),
        subChunk.blockEntityPayload.end()
    );
    ensureTrackedColumn(subChunk.chunkX, subChunk.chunkZ)
        .networkDecodeSubChunkNoCache(subChunk.sectionY, payload);
    pendingBlobMetadata_.erase(subChunk.hash);
    return true;
}

BedrockChunkColumn& BedrockNetworkClient::ensureTrackedColumn(
    int32_t chunkX,
    int32_t chunkZ
) {
    if (auto* column = world_.getLoadedColumn(chunkX, chunkZ)) {
        return *column;
    }

    BedrockChunkColumn column(chunkX, chunkZ);
    column.setBounds(-4, 20);
    world_.setLoadedColumn(chunkX, chunkZ, std::move(column));
    auto* inserted = world_.getLoadedColumn(chunkX, chunkZ);
    if (inserted == nullptr) {
        throw BedrockChunkError("failed to create tracked chunk column");
    }
    return *inserted;
}

void BedrockNetworkClient::prepareLoginPacket() {
    BedrockNetworkClientOptions authOptions;
    std::filesystem::path effectiveAuthenticationCacheRoot;
    MsalConfigPtr effectiveMsalConfig;
    std::shared_ptr<MicrosoftAuthFlow> builtInMicrosoftAuthFlow;
    {
        std::lock_guard<std::mutex> lock(optionsMutex_);
        authOptions = options_;
        effectiveAuthenticationCacheRoot = authenticationCacheRoot_;
        effectiveMsalConfig = authenticationMsalConfig_;
        builtInMicrosoftAuthFlow = authenticationMicrosoftAuthFlow_;
    }
    if (!authOptions.loginPacket.empty()) {
        if (clientKeys_.privateKeyPem.empty()) {
            clientKeys_ = XboxLiveAuth::loadOrCreateProfileKeyPair(
                authOptions.profile,
                authOptions.authCacheRoot
            );
        }
        return;
    }

    XboxLiveAuthOptions loginOptions {
        .profileName = authOptions.profile,
        .version = authOptions.version,
        .protocolVersion = session_.definition().protocolVersion(),
        .serverAddress = authOptions.host + ":" + std::to_string(authOptions.port),
        .offline = authOptions.offline,
        .interactiveAuth = authOptions.interactiveAuth,
        .authTitle = authOptions.authTitle,
        .deviceType = authOptions.deviceType,
        .flow = authOptions.flow,
        .forceRefresh = authOptions.forceRefresh,
        .msalConfig = authOptions.msalConfig,
        .xboxClientId = authOptions.xboxClientId,
        // Online Authflow owns the canonical profilesFolder-derived path.
        // The legacy native alias remains available only to the native offline
        // extension and never mutates the JavaScript-visible options object.
        .cacheRoot = authOptions.offline
            ? authOptions.authCacheRoot
            : effectiveAuthenticationCacheRoot,
        .clientDataJson = authOptions.clientDataJson,
        .onMsaCode = authOptions.onMsaCode,
        .onDeviceCode = [](const XboxDeviceCodeInfo& info) {
            std::cout << "[XBOX] Open: " << info.verificationUri << "\n";
            std::cout << "[XBOX] Code: " << info.userCode << "\n";
            if (!info.message.empty()) {
                std::cout << "[XBOX] " << info.message << "\n";
            }
        },
        .onLog = [](const std::string& message) {
            std::cout << "[XBOX] " << message << "\n";
        }
    };

    XboxLiveLoginPacket generated;
    if (authOptions.offline) {
        // The offline session plugin wins before auth.js is considered. A
        // runtime authflow property may still exist (notably after Realm
        // address resolution), but no online Promise was started for it.
        generated = XboxLiveAuth::makeLoginPacketFromPreparedFlow(
            std::move(loginOptions),
            std::move(effectiveMsalConfig)
        );
    } else if (authOptions.authflow) {
        std::future<std::vector<std::string>> pendingChains;
        BedrockClientKeyPair authflowKeys;
        {
            std::lock_guard<std::mutex> lock(optionsMutex_);
            pendingChains = std::move(pendingAuthflowChains_);
            authflowKeys = clientKeys_;
        }
        if (!pendingChains.valid()) {
            throw std::runtime_error(
                "authflow.getMinecraftBedrockToken(...).catch is not a function"
            );
        }
        std::vector<std::string> chains;
        try {
            chains = pendingChains.get();
        } catch (const std::exception& error) {
            // auth.js's inner Promise.catch emits the password warning only
            // for rejection of getMinecraftBedrockToken itself. Later chain
            // parsing/build failures skip that warning.
            throw AuthflowPromiseRejection(error.what());
        }
        generated = XboxLiveAuth::makeLoginPacketFromChains(
            std::move(loginOptions),
            std::move(authflowKeys),
            std::move(chains)
        );
    } else if (builtInMicrosoftAuthFlow) {
        BedrockClientKeyPair authflowKeys;
        {
            std::lock_guard<std::mutex> lock(optionsMutex_);
            if (clientKeys_.privateKeyPem.empty()) {
                clientKeys_ = BedrockAuthJwt::generateP384KeyPair();
            }
            authflowKeys = clientKeys_;
        }
        JsRuntimeValue runtimeChains;
        try {
            runtimeChains = builtInMicrosoftAuthFlow->
                getMinecraftBedrockToken(JsRuntimeValue::string(
                    authflowKeys.publicKeyDerBase64
                )).get();
        } catch (const std::exception& error) {
            throw AuthflowPromiseRejection(error.what());
        }
        if (!runtimeChains.isArray()) {
            throw std::runtime_error(
                "getMinecraftBedrockToken did not return an array"
            );
        }
        std::vector<std::string> chains;
        chains.reserve(runtimeChains.length());
        for (std::size_t index = 0; index < runtimeChains.length(); ++index) {
            const auto* token = runtimeChains.get(index);
            if (!token || !token->isString()) {
                throw std::runtime_error(
                    "Bedrock authentication chain contains a non-string token"
                );
            }
            chains.push_back(token->stringValue());
        }
        generated = XboxLiveAuth::makeLoginPacketFromChains(
            std::move(loginOptions),
            std::move(authflowKeys),
            std::move(chains)
        );
    } else {
        generated = XboxLiveAuth::makeLoginPacketFromPreparedFlow(
            std::move(loginOptions),
            std::move(effectiveMsalConfig)
        );
    }

    {
        std::lock_guard<std::mutex> lock(optionsMutex_);
        options_.loginPacket = std::move(generated.loginPacket);
    }
    clientKeys_ = std::move(generated.keyPair);
}

void BedrockNetworkClient::sendLogin() {
    setStatus(BedrockNetworkClientStatus::Authenticating);
    prepareLoginPacket();
    auto packet = session_.packetCodec().decodeFullPacket(
        options().loginPacket
    );
    sendPackets({packet});
}

void BedrockNetworkClient::startEncryptionFromServerHandshake(const std::string& token) {
    if (token.empty()) {
        throw std::runtime_error("server_to_client_handshake has no token");
    }

    auto encryptionKeys = BedrockKeyExchange::deriveFromServerHandshakeJwtAndPrivateKeyPem(
        token,
        clientKeys_.privateKeyPem
    );
    auto encryptStream = BedrockEncryption::createCipherStream(
        options_.protocolVersion,
        encryptionKeys.secretKeyBytes,
        encryptionKeys.iv16,
        BedrockCipherMode::Encrypt
    );
    auto decryptStream = BedrockEncryption::createCipherStream(
        options_.protocolVersion,
        encryptionKeys.secretKeyBytes,
        encryptionKeys.iv16,
        BedrockCipherMode::Decrypt
    );

    auto handshake = session_.writeClientToServerHandshake();
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        encryptionKeys_ = std::move(encryptionKeys);
        encryptStream_ = std::move(encryptStream);
        decryptStream_ = std::move(decryptStream);
        sendCounter_ = 0;
        receiveCounter_ = 0;
        encryptionEnabled_ = true;
        // The handshake must be the first encrypted packet. Holding the same
        // lock as the queue pump prevents an interval flush from overtaking it.
        sendPacketsLocked({handshake});
    }
    session_.takeOutgoingPackets();

    emitJoin();
    setStatus(BedrockNetworkClientStatus::Initializing);
}

void BedrockNetworkClient::drainSessionOutgoing() {
    auto outgoing = session_.takeOutgoingPackets();
    if (!outgoing.empty()) {
        sendPackets(outgoing);
    }
}

bool BedrockNetworkClient::startQueue(
    bool pumpEnabled,
    bool allowAfterTransportStop
) {
    std::lock_guard<std::mutex> lifecycleLock(queueLifecycleMutex_);
    // Serialize start admission with stopQueue(). If close published its stop
    // before this lock was acquired, no post-close queue thread is created. If
    // start won first, close necessarily acquires this lock next and joins it.
    if (rakNetStopRequested_.load() && !allowAfterTransportStop) return false;
    // This is an internal lifecycle replacement, not a public stop. Preserve
    // a failed-close catch-boundary deadline installed before or during the
    // old worker's join; the fresh queue must not become runnable inside that
    // synchronous exception handoff.
    stopQueueLocked(true);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (rakNetStopRequested_.load() && !allowAfterTransportStop) {
            return false;
        }
        const bool closeEmissionActive = closing_.load();
        const bool rollbackBoundaryPending =
            queuePumpResumeDue_.has_value();
        stopQueue_ = false;
        queuePumpRequested_ = pumpEnabled;
        queuePumpEnabled_ = pumpEnabled && !closeEmissionActive &&
            !rollbackBoundaryPending;
        queuePumpInFlight_ = false;
        if (closeEmissionActive) {
            queuePumpResumeDue_.reset();
        }
        tick_ = 0;
    }
    const auto provider = callbackLifetimeProviderSnapshot();
    try {
        queueThread_ = std::thread([this, provider]() {
            // A queue-thread callback (heartbeat/send seam/user listener) may
            // destroy the last facade owner. Keep State -> network alive until
            // queueLoop has observed stopQueue_ and returned from every `this`
            // access, mirroring the RakNet worker's transport-wide lease.
            auto threadLease = provider ? provider() : std::shared_ptr<void>();
            if (provider && !threadLease) return;
            queueLoop();
        });
    } catch (...) {
        // Do not leave a logically running queue with no worker if native
        // thread creation fails. The caller's lifecycle phase guard will roll
        // the connect preparation back before propagating this exception.
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopQueue_ = true;
        queuePumpRequested_ = false;
        queuePumpEnabled_ = false;
        queuePumpInFlight_ = false;
        throw;
    }
    return true;
}

void BedrockNetworkClient::resumeQueuePump() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopQueue_) return;
        queuePumpRequested_ = true;
        if (closing_.load() || queuePumpResumeDue_.has_value()) {
            // A close listener is a synchronous JavaScript boundary. Retain
            // demand, but do not arm the independent C++ timer within it. A
            // rollback (or an already-published rollback due time) performs
            // the later activation.
            queuePumpEnabled_ = false;
        } else {
            queuePumpEnabled_ = true;
        }
    }
    queueCv_.notify_all();
}

bool BedrockNetworkClient::pauseQueuePump() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    // Logical demand survives physical suppression. This lets nested or
    // back-to-back throwing close frames clear a prior due time without
    // losing the timer that the outermost rollback must re-schedule; an
    // intentionally paused factory queue has requested=false.
    const bool wasEnabled = queuePumpRequested_;
    queuePumpEnabled_ = false;
    queuePumpResumeDue_.reset();
    queueCv_.notify_all();
    queueCv_.wait(lock, [this]() { return !queuePumpInFlight_; });
    return wasEnabled;
}

void BedrockNetworkClient::stopQueue() {
    std::lock_guard<std::mutex> lifecycleLock(queueLifecycleMutex_);
    stopQueueLocked();
}

void BedrockNetworkClient::stopQueueIfRunning() {
    std::lock_guard<std::mutex> lifecycleLock(queueLifecycleMutex_);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopQueue_) return;
    }
    stopQueueLocked();
}

void BedrockNetworkClient::stopQueueLocked(bool preserveRollbackDeadline) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        stopQueue_ = true;
        queuePumpRequested_ = false;
        queuePumpEnabled_ = false;
        if (!preserveRollbackDeadline) queuePumpResumeDue_.reset();
        queuedPackets_.clear();
        chunkRadiusDue_.reset();
        keepAliveDue_.reset();
    }
    queueCv_.notify_all();

    if (queueThread_.joinable()) {
        if (queueThread_.get_id() == std::this_thread::get_id()) {
            queueThread_.detach();
        } else {
            queueThread_.join();
        }
    }
}

void BedrockNetworkClient::queueLoop() {
    const auto interval = std::chrono::milliseconds(
        options_.batchingIntervalMs > 0 ? options_.batchingIntervalMs : 20
    );
    auto nextTick = std::chrono::steady_clock::now() + interval;

    std::unique_lock<std::mutex> lock(queueMutex_);
    while (!stopQueue_) {
        if (!queuePumpEnabled_) {
            if (!queuePumpResumeDue_.has_value()) {
                queueCv_.wait(lock, [this]() {
                    return stopQueue_ || queuePumpEnabled_ ||
                        queuePumpResumeDue_.has_value();
                });
            } else {
                const auto resumeDue = *queuePumpResumeDue_;
                queueCv_.wait_until(lock, resumeDue, [this, resumeDue]() {
                    return stopQueue_ || queuePumpEnabled_ ||
                        !queuePumpResumeDue_.has_value() ||
                        *queuePumpResumeDue_ != resumeDue;
                });
                if (!stopQueue_ && !queuePumpEnabled_ &&
                    queuePumpResumeDue_.has_value() &&
                    std::chrono::steady_clock::now() >=
                        *queuePumpResumeDue_) {
                    queuePumpEnabled_ = queuePumpRequested_ &&
                        !closing_.load();
                    queuePumpResumeDue_.reset();
                }
            }
            continue;
        }
        if (std::chrono::steady_clock::now() < nextTick) {
            queueCv_.wait_until(lock, nextTick, [this]() {
                return stopQueue_ || !queuePumpEnabled_;
            });
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        nextTick = now + interval;
        if (chunkRadiusDue_.has_value() && now >= *chunkRadiusDue_) {
            auto payload = packetEncoder_.encodePacket(
                "request_chunk_radius",
                ProtoDefValue::object({
                    {"chunk_radius", ProtoDefValue::integer(options_.chunkRadius)},
                    // JS leaves this field undefined; protodef's u8 writer
                    // coerces that value to the same wire byte 0x00.
                    {"max_radius", ProtoDefValue::uinteger(0)}
                })
            );
            queuedPackets_.push_back(
                session_.packetCodec().makePacketByName("request_chunk_radius", payload)
            );
            chunkRadiusDue_.reset();
        }

        if (keepAliveDue_.has_value() && now >= *keepAliveDue_) {
            auto payload = packetEncoder_.encodePacket(
                "tick_sync",
                ProtoDefValue::object({
                    {"request_time", ProtoDefValue::integer(tick_)},
                    {"response_time", ProtoDefValue::integer(0)}
                })
            );
            queuedPackets_.push_back(
                session_.packetCodec().makePacketByName("tick_sync", payload)
            );
            tick_ += 10;
            keepAliveDue_ = now + std::chrono::milliseconds(500);
        }

        auto packets = std::move(queuedPackets_);
        queuedPackets_.clear();
        queuePumpInFlight_ = true;
        lock.unlock();
        try {
            sendPackets(packets);
        } catch (...) {
            lock.lock();
            queuePumpInFlight_ = false;
            queueCv_.notify_all();
            throw;
        }
        lock.lock();
        queuePumpInFlight_ = false;
        queueCv_.notify_all();
    }
}

void BedrockNetworkClient::resetLifecycle() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimeEntityId_.reset();
        initializeOnNextStartGame_ = false;
    }
    resourcePacksInfoHandled_ = false;
    resourcePackStackHandlerArmed_ = false;
    compressionReady_ = false;
}

void BedrockNetworkClient::sendLocalPlayerInitialized(uint64_t runtimeEntityId) {
    write("set_local_player_as_initialized", ProtoDefValue::object({
        {"runtime_entity_id", ProtoDefValue::uinteger(runtimeEntityId)}
    }));
}

void BedrockNetworkClient::sendPackets(const std::vector<VersionedGamePacket>& packets) {
    for (const auto& packet : packets) {
        if (packet.name == "start_game" || packet.name == "item_registry") {
            (void) packetDecoder_.decodePacket(packet.name, packet.payload);
        }
    }
    std::lock_guard<std::mutex> lock(sendMutex_);
    sendPacketsLocked(packets);
}

void BedrockNetworkClient::sendPacketsLocked(
    const std::vector<VersionedGamePacket>& packets
) {
    if (packets.empty() || (!raknet_ && !reliableSendOverride_)) {
        return;
    }

    if (encryptionEnabled_) {
        if (!encryptStream_) {
            throw std::runtime_error("encrypt stream is not initialized");
        }

        // encryption.js receives Framer#getBuffer and always raw-deflates it;
        // neither the ordinary Framer threshold nor its compressor selection
        // participates in the encrypted wire format.
        auto compressionPacket = session_.mcpeCodec().encodeEncryptedCompressionPacket(
            packets,
            options_.compressionLevel
        );

        auto aesPlaintext = BedrockEncryption::makeAesPlaintext(
            compressionPacket,
            sendCounter_++,
            encryptionKeys_.secretKeyBytes
        );

        auto encryptedOnly = encryptStream_->process(aesPlaintext);

        std::vector<uint8_t> encrypted;
        encrypted.reserve(1 + encryptedOnly.size());
        encrypted.push_back(0xfe);
        encrypted.insert(encrypted.end(), encryptedOnly.begin(), encryptedOnly.end());

        sendReliablePayload(encrypted);
        return;
    }

    if (session_.mcpeCodec().compressorInPacketHeader() && !compressionReady_) {
        // Framer writes no compressor byte at all before network_settings.
        // Passing Uncompressed to the negotiated codec would instead prepend
        // 0xff, which the peer correctly interprets as the first batch varint.
        auto framed = session_.batchCodec().encodeFramedBatch(packets);
        std::vector<uint8_t> mcpe;
        mcpe.reserve(1 + framed.size());
        mcpe.push_back(0xfe);
        mcpe.insert(mcpe.end(), framed.begin(), framed.end());
        sendReliablePayload(mcpe);
        return;
    }

    auto compression = choosePlainCompression(packets);
    auto mcpe = session_.mcpeCodec().encodeMcpePayload(
        packets,
        compression,
        options_.compressionLevel
    );
    sendReliablePayload(mcpe);
}

void BedrockNetworkClient::sendReliablePayload(
    const std::vector<uint8_t>& payload
) {
    if (reliableSendOverride_) {
        reliableSendOverride_(payload);
        return;
    }
    if (raknet_) {
        raknet_->sendReliable(payload);
    }
}

VersionedMcpeCompression BedrockNetworkClient::choosePlainCompression(
    const std::vector<VersionedGamePacket>& packets
) const {
    if (compressionAlgorithm_ == "none") {
        return VersionedMcpeCompression::Uncompressed;
    }

    if (!compressionReady_ && versionAtLeast(options_.version, 1, 19, 30)) {
        return VersionedMcpeCompression::Uncompressed;
    }

    auto framed = session_.batchCodec().encodeFramedBatch(packets);
    if (framed.size() > compressionThreshold_) {
        return compressionAlgorithm_ == "snappy"
            ? VersionedMcpeCompression::Snappy
            : VersionedMcpeCompression::DeflateRaw;
    }

    return VersionedMcpeCompression::Uncompressed;
}

} // namespace bedrock
