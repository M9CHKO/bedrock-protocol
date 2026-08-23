#include <bedrock/bedrock.hpp>

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
        bedrock::BedrockServer direct({
            .host = "127.0.0.1",
            .port = 0,
            .version = "1.26.0",
            .motd = {{"motd", "direct constructor"}}
        });
        ok &= check(
            !direct.listening() && direct.boundPort() == 0,
            "direct BedrockServer construction unexpectedly called listen()"
        );

        // Let the OS select an unused port, then reuse it to exercise the
        // public factory without relying on a fixed test port.
        direct.listen();
        const uint16_t port = direct.boundPort();
        ok &= check(port != 0, "ephemeral port allocation failed");
        direct.close();

        auto server = bedrock::createServer({
            .host = "127.0.0.1",
            .port = port,
            .version = "1.26.0",
            .motd = {{"motd", "createServer auto-listen"}}
        });

        ok &= check(
            server.listening() && server.boundPort() == port,
            "createServer did not synchronously start the RakNet backend"
        );

        const auto advertisement = bedrock::ping({
            .host = "127.0.0.1",
            .port = port
        });
        ok &= check(
            advertisement.motd == "createServer auto-listen",
            "auto-started server did not answer an unconnected ping"
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
