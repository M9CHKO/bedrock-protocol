#include <bedrock/relay/MapRequestQueue.hpp>
#include <bedrock/relay/BedrockLiveRelay.hpp>
#include <atomic>
#include <iostream>
using namespace bedrock;
using namespace std::chrono_literals;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static VersionedGamePacket request(const std::string& version, int64_t id, bool pixel = false) {
    std::vector<uint8_t> data; MapImageCodec::zig(data, id);
    data.insert(data.end(), {uint8_t(pixel), 0, 0, 0});
    if (pixel) data.insert(data.end(), {1, 2, 3, 4, 5, 0});
    return VersionedPacketCodec::forVersion(version).makePacketByName("map_info_request", data);
}
template<class F> static bool waitFor(F f) {
    auto end = std::chrono::steady_clock::now() + 12s;
    while (!f() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(10ms);
    return f();
}
static void unit(const std::string& version) {
    MapRequestQueue q(version); auto t = MapRequestQueue::Clock::now() + 1h;
    auto p = request(version, -123);
    for (int i=0;i<100;++i) require(q.intercept(p,t), "request always consumed");
    require(q.stats().pending==1 && q.stats().duplicate==99, "identical request deduplication");
    require(!q.pump([](const auto&){return true;},true,0,t), "No Render pauses backend requests");
    require(!q.pump([](const auto&){return true;},false,4,t), "image backlog stops new requests");
    require(q.pump([&](const auto& out){require(out.fullPacket==p.fullPacket,"request byte fidelity");return true;},false,0,t), "request release");
    require(!q.pump([](const auto&){return true;},false,0,t), "no same-tick burst");
    q.intercept(request(version,2),t); q.intercept(request(version,3),t);
    require(q.pump([](const auto&){return true;},false,0,t+250ms), "second request");
    require(!q.pump([](const auto&){return true;},false,0,t+500ms), "max two unanswered maps");
    q.response(-123);
    require(q.pump([](const auto&){return true;},false,0,t+500ms), "response releases in-flight slot");
    q.reset(); q.intercept(p,t+10s); q.intercept(request(version,-123,true),t+10s);
    require(q.stats().pending==2,"different client_pixels must not coalesce");
    require(!q.pump([&](const auto&){q.reset();return false;},false,0,t+10s) && q.stats().pending==0,
        "reset during send does not replay request");
    auto bad=p;bad.payload.pop_back();bad.fullPacket.pop_back();
    require(q.intercept(bad,t+11s) && q.stats().malformed==1,"truncated request consumed");
    for(size_t id=0;id<MapRequestQueue::MaxEntries+500;++id) q.intercept(request(version,id),t+11s);
    require(q.stats().pending<=MapRequestQueue::MaxEntries && q.stats().bytes<=MapRequestQueue::MaxBytes && q.stats().overflow>0,
        "bounded intake under request flood");
    MapRequestQueue retry(version); retry.intercept(p,t);
    require(!retry.pump([](const auto&){return false;},false,0,t) && retry.stats().pending==1,
        "backpressure retains request");
    require(retry.pump([](const auto&){return true;},false,0,t),"backpressure recovery");
    retry.intercept(p,t+1s); require(retry.stats().pending==0,"in-flight identical duplicate suppressed");
    retry.intercept(p,t+6s); require(retry.stats().pending==1,"new demand allowed after duplicate window");
    require(retry.pump([](const auto&){return true;},false,0,t+6s),"expired flight releases slot");
    retry.response(-123); retry.intercept(request(version,7),t+6s);
    require(!retry.pump([](const auto&){return true;},false,0,t+6s),"no catch-up after pause");
    MapRequestQueue bounded(version);
    auto codec=VersionedPacketCodec::forVersion(version);
    for(int id=0;id<10;++id) {
        std::vector<uint8_t> payload; MapImageCodec::zig(payload,id);
        payload.insert(payload.end(),{0,64,0,0}); payload.resize(payload.size()+16384*6,0);
        bounded.intercept(codec.makePacketByName("map_info_request",payload),t);
    }
    require(bounded.stats().pending==2 && bounded.stats().overflow==8 && bounded.stats().bytes<=MapRequestQueue::MaxBytes,
        "large pixel requests obey byte limit independently of count");
    auto invalid=request(version,1,true); invalid.payload.back()=64;
    invalid=codec.makePacketByName("map_info_request",invalid.payload);
    bounded.intercept(invalid,t); require(bounded.stats().malformed==1,"out of range pixel rejected");
    auto mismatched=p; mismatched.fullPacket=codec.makePacketByName("animate",{2,22}).fullPacket;
    bounded.intercept(mismatched,t); require(bounded.stats().malformed==2,"wire mismatch consumed");
    auto ordinary=codec.makePacketByName("animate",{2,22});
    require(!bounded.intercept(ordinary,t),"ordinary gameplay bypasses request queue");
}
static void live(const std::string& version) {
    std::atomic<int> requests{0}, actions{0}, maps{0}; std::atomic<bool> overload{false};
    std::mutex mutex; std::deque<std::chrono::steady_clock::time_point> times;
    BedrockServerConnection target;
    std::atomic<bool> firstReport{false}, finalReport{false};
    BedrockServer backend({.host="127.0.0.1",.port=0,.version=version,.offline=true});
    backend.on("map_info_request",[&](const BedrockServerPacketEvent& e){
        bool tooMany;
        { std::lock_guard lock(mutex); auto now=std::chrono::steady_clock::now();
          while(!times.empty() && now-times.front()>=980ms) times.pop_front();
          times.push_back(now); tooMany=times.size()>MapSendBudget::PacketsPerSecond; target=e.connection; }
        ++requests;
        if(tooMany) {overload=true;backend.closeConnection(e.connection);return;}
        auto id=MapImageCodec::id(e.packet.payload).value();
        MapImageCodec::State state; state.id=id; state.common={0,0,0,0,0}; state.included={0}; state.tracked={0,0};
        backend.sendPacket(e.connection,MapImageCodec(version).packet(state,false));
    });
    backend.on("animate",[&](const BedrockServerPacketEvent&){++actions;}); backend.listen();
    BedrockLiveRelayOptions options; options.server.host="127.0.0.1";options.server.port=0;
    options.server.version=version;options.server.offline=true;options.upstream.host="127.0.0.1";
    options.upstream.port=backend.boundPort();options.upstream.version=version;options.upstream.offline=true;
    BedrockLiveRelay relay(options); relay.onError([](const auto& e){std::cerr<<e<<'\n';});
    relay.onDiagnostic([&](const std::string& s){
        if(s.find("requestsSubmitted=")!=std::string::npos) firstReport=true;
        if(s.find("final=1")!=std::string::npos) finalReport=true;
    });
    relay.listen();
    auto client=createNetworkClient({.host="127.0.0.1",.port=relay.boundPort(),.username="MapDemandTest",.version=version,.offline=true});
    client.on("clientbound_map_item_data",[&](const auto&){++maps;});
    auto swing=VersionedPacketCodec::forVersion(version).makePacketByName("animate",{2,22});
    require(client.connect() && waitFor([&]{return relay.upstreamReady();}),"live connect");
    for(int id=1;id<=12;++id) for(int repeat=0;repeat<5;++repeat) client.sendBuffer(request(version,id).fullPacket);
    client.sendPacket(swing);
    require(waitFor([&]{return actions>0;}) && requests<12,"gameplay overtakes request backlog");
    require(waitFor([&]{return requests>=5 || overload.load();}),"requests progress");
    require(!overload && relay.upstreamReady(),"backend rejected map request burst");
    require(firstReport,"first demand diagnostics visible before periodic delay");
    require(waitFor([&]{return requests==12 && maps==12 && actions>0;}),"paced maps and gameplay complete");
    relay.configureNoRender(true,false);
    for(int id=13;id<=16;++id) client.sendBuffer(request(version,id).fullPacket);
    std::this_thread::sleep_for(750ms);
    require(requests==12,"hidden maps must not trigger backend requests");
    client.sendPacket(swing); require(waitFor([&]{return actions>=2;}),"actions work while requests paused");
    relay.configureNoRender(false,false);
    require(waitFor([&]{return requests==16;}),"unhide resumes held requests");
    require(!overload && relay.upstreamReady(),"session survives request flood");
    relay.configureNoRender(true,false);
    for(int id=17;id<=24;++id) client.sendBuffer(request(version,id).fullPacket);
    BedrockServerConnection closing; {std::lock_guard lock(mutex);closing=target;}
    backend.closeConnection(closing);
    require(waitFor([&]{return finalReport && !relay.upstreamReady();}),"remote close reports and resets held demand");
    require(relay.listening(),"remote close leaves listener alive");
    client.close();relay.close();backend.close();
}
int main(int argc,char** argv) {
    try {
        for(const std::string version:{"1.21.2","1.21.100"}) {
            if(argc>1 && std::string(argv[1])=="--live") live(version); else unit(version);
        }
        std::cout<<"map request queue: OK\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
