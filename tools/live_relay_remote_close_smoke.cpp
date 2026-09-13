#include <bedrock/relay/BedrockLiveRelay.hpp>
#include <bedrock/server/BedrockServer.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
template<class F> bool waitFor(F fn) {
    const auto end = std::chrono::steady_clock::now() + 8s;
    while (!fn() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(10ms);
    return fn();
}

// Keep the relay listening, but let the BACKEND terminate each session. Do not
// retain the relay's network client: that would mask callback lifetime bugs.
static bool simultaneousClose(const std::string& version) {
    bedrock::BedrockServer backend({.host="127.0.0.1",.port=0,.version=version,.offline=true});
    std::mutex mutex;
    bedrock::BedrockServerConnection target;
    std::atomic<bool> joined{false}, publicClosing{false}, kickReceived{false}, finished{false}, orderingOk{true};
    backend.onJoin([&](const auto& c){std::lock_guard lock(mutex);target=c;joined=true;});
    backend.listen();
    bedrock::BedrockNetworkClient client({.host="127.0.0.1",.port=backend.boundPort(),
        .username="CloseRaceTest",.version=version,.offline=true});
    client.on("kick",[&](const bedrock::BedrockNetworkClientPacketEvent&){kickReceived=true;});
    client.onClose([&](const std::string&){publicClosing=true;if(!waitFor([&]{return kickReceived.load();})) orderingOk=false;});
    if(!client.connect() || !waitFor([&]{return joined.load();})) return false;
    std::thread closer([&]{client.close();finished=true;});
    if(!waitFor([&]{return publicClosing.load();})) std::abort();
    bedrock::BedrockServerConnection connection; {std::lock_guard lock(mutex);connection=target;}
    backend.disconnect(connection,"simultaneous close test");
    // A deadlock must fail the regression promptly, not hang in a destructor.
    if(!waitFor([&]{return finished.load();})) {std::cerr<<"public/remote close deadlock\n";std::abort();}
    closer.join();backend.close();return orderingOk && kickReceived;
}

int main() {
    for (const std::string version : {"1.21.2", "1.21.100"}) {
        if(!simultaneousClose(version)) return 5;
        bedrock::BedrockServer backend({.host="127.0.0.1", .port=0, .version=version, .offline=true});
        std::mutex mutex;
        std::atomic<int> backendJoins{0};
        bedrock::BedrockServerConnection target;
        backend.onJoin([&](const auto& connection) { std::lock_guard lock(mutex); target=connection; ++backendJoins; });
        backend.listen();
        bedrock::BedrockLiveRelayOptions options;
        options.server.host="127.0.0.1"; options.server.port=0;
        options.server.version=version; options.server.offline=true;
        options.upstream.host="127.0.0.1"; options.upstream.port=backend.boundPort();
        options.upstream.version=version; options.upstream.offline=true;
        options.upstream.connectTimeoutMs=3000;
        bedrock::BedrockLiveRelay relay(options);
        std::atomic<int> joins{0}, closes{0};
        std::weak_ptr<bedrock::BedrockNetworkClient> retired;
        relay.onUpstreamJoin([&](const auto&, const auto& network) {
            { std::lock_guard lock(mutex); retired=network; }
            ++joins;
        });
        relay.onDisconnect([&](const auto&) { ++closes; });
        relay.onError([](const auto& error) { std::cerr << error << std::endl; });
        relay.listen();
        for (int cycle=0; cycle<6; ++cycle) {
            std::cout << "remote-close " << version << " cycle=" << cycle << std::endl;
            bedrock::BedrockNetworkClient client({.host="127.0.0.1", .port=relay.boundPort(),
                .username="RemoteCloseTest", .version=version, .offline=true, .connectTimeoutMs=3000});
            client.onError([](const auto& error) { std::cerr << error << std::endl; });
            if (!client.connect() || !waitFor([&]{ return joins==cycle+1 && backendJoins==cycle+1; })) return 1;
            bedrock::BedrockServerConnection connection;
            { std::lock_guard lock(mutex); connection=target; }
            // Transport notification and game-level disconnect take different
            // native close stacks. Both must release the owner after unwinding.
            if (cycle%2==0) backend.closeConnection(connection);
            else backend.disconnect(connection, "remote-close test");
            if (!waitFor([&]{ return closes==cycle+1 && relay.sessionCount()==0 && backend.clientCount()==0; })) {
                std::cerr << "close timeout closes=" << closes << " sessions=" << relay.sessionCount()
                          << " backend=" << backend.clientCount() << std::endl;
                return 2;
            }
            if (!waitFor([&]{ std::lock_guard lock(mutex); return retired.expired(); })) return 3;
            if (relay.finalSessionResetCount()!=static_cast<uint64_t>(cycle+1) || !relay.listening()) return 4;
            client.close();
            std::this_thread::sleep_for(50ms);
        }
        relay.close(); backend.close();
    }
    std::cout << "remote-close/reconnect: OK" << std::endl;
}
