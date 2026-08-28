#include <bedrock/bedrock.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[CREATE-SERVER-FACTORY-SMOKE] " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    bool ok = true;

    try {
        std::atomic<int> advertisementCalls {0};
        bedrock::BedrockServer direct({
            .host = "127.0.0.1",
            .port = 0,
            .version = "1.26.0",
            .motd = {{"motd", "direct constructor"}},
            .advertisementFn = [&]() {
                ++advertisementCalls;
                return bedrock::ServerAdvertisement({
                    {"motd", "value advertisement callback"},
                    {"playersOnline", 2},
                    {"playersMax", 8}
                }, 0, "1.26.0");
            }
        });
        ok &= check(
            !direct.listening() && direct.boundPort() == 0,
            "direct BedrockServer construction unexpectedly called listen()"
        );

        // Let the OS select an unused port, then reuse it to exercise the
        // public factory without relying on a fixed test port.
        const auto directAddress = direct.listen();
        const uint16_t port = direct.boundPort();
        ok &= check(
            directAddress.host == "127.0.0.1" && directAddress.port == 0,
            "listen() did not return the configured JavaScript host/port"
        );
        ok &= check(port != 0, "ephemeral port allocation failed");
        const auto directAdvertisement = bedrock::ping({
            .host = "127.0.0.1",
            .port = port
        });
        ok &= check(
            advertisementCalls.load() >= 1 &&
                directAdvertisement.motd == "value advertisement callback" &&
                directAdvertisement.playersOnline == 2 &&
                directAdvertisement.playersMax == 8,
            "value-returning advertisementFn was not published"
        );
        direct.close();

        auto server = bedrock::createServer({
            .host = "127.0.0.1",
            .port = port,
            .version = "1.26.0",
            .motd = {"createServer auto-listen", "factory world"}
        });

        ok &= check(
            server.listening() && server.boundPort() == port,
            "createServer did not synchronously start the RakNet backend"
        );
        ok &= check(
            server.clients().empty(),
            "new Server clients snapshot was not empty"
        );

        const auto advertisement = bedrock::ping({
            .host = "127.0.0.1",
            .port = port
        });
        ok &= check(
            advertisement.motd == "createServer auto-listen",
            "auto-started server did not answer an unconnected ping"
        );
        ok &= check(
            advertisement.levelName == "factory world",
            "compact ServerMotd did not publish its level name"
        );

        server.close();
    } catch (const std::exception& error) {
        std::cerr << "[CREATE-SERVER-FACTORY-SMOKE] threw: "
                  << error.what() << "\n";
        ok = false;
    }

    if (!ok) {
        return 1;
    }

    std::cout << "[CREATE-SERVER-FACTORY-SMOKE] ok\n";
    return 0;
}
