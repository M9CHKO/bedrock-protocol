// Diagnostic probe: only synthetic cache/credentials, never a user's profile.
#include <cpe/WindowsHttp.hpp>
#include <bedrock/auth/NativeBedrockAuthflow.hpp>
#include <bedrock/auth/FileAuthCache.hpp>
#include <chrono>
#include <iostream>
#include <thread>

class SyntheticHttp final : public bedrock::IXboxTokenHttpClient {
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue;
public:
    explicit SyntheticHttp(std::shared_ptr<bedrock::JsMicrotaskQueue> value) : queue(std::move(value)) {}
    bedrock::JsPromise<bedrock::XboxTokenHttpResponse> fetch(bedrock::XboxTokenHttpRequest) override {
        std::cout << "synthetic_http_reached" << std::endl;
        return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::fromSynchronous(queue, [] {
            bedrock::XboxTokenHttpResponse result;
            result.status = 200;
            result.bodyText = R"({"device_code":"synthetic","user_code":"synthetic","verification_uri":"https://example.invalid/","expires_in":0,"interval":0})";
            return result;
        });
    }
};
int main(int argc, char** argv) {
    std::thread([] { std::this_thread::sleep_for(std::chrono::seconds(25)); std::cerr << "probe_timeout" << std::endl; std::_Exit(124); }).detach();
    try {
        if (argc == 2 && std::string(argv[1]) == "--http") {
            cpe::WindowsTokenHttpClient http(bedrock::JsMicrotaskQueue::create());
            bedrock::XboxTokenHttpRequest request;
            request.method = "post"; request.url = "https://login.live.com/oauth20_connect.srf";
            request.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
            std::cout << "unauthenticated_http_probe_started" << std::endl;
            const auto response = http.fetch(std::move(request)).get();
            std::cout << "http_status=" << response.status << " body_omitted" << std::endl;
            return response.status >= 400 && response.status < 500 ? 0 : 1;
        }
        if (argc != 2) return 2;
        std::filesystem::create_directories(argv[1]);
        bedrock::NativeBedrockAuthflowOptions options;
        options.username = "synthetic";
        options.profilesFolder = std::filesystem::path(argv[1]);
        options.httpClientFactory = [](auto queue) { return std::make_shared<SyntheticHttp>(std::move(queue)); };
        bool codeSeen = false;
        options.onMsaCode = [&](const auto&) { codeSeen = true; std::cout << "synthetic_device_code_callback" << std::endl; };
        bedrock::validateNativeBedrockAuthflowOptions(options);
        std::cout << "creating_auth_runtime" << std::endl;
        auto runtime = bedrock::createNativeBedrockAuthflow(options, std::filesystem::path(argv[1]));
        std::cout << "requesting_synthetic_login" << std::endl;
        try { runtime.microsoft->getMinecraftBedrockToken(bedrock::JsRuntimeValue::string("synthetic-key")).get(); }
        catch (const std::exception& error) { std::cout << "synthetic_login_rejected: " << error.what() << std::endl; }
        return codeSeen ? 0 : 1;
    } catch (const std::exception&) { std::cerr << "probe_failed_details_omitted" << std::endl; return 1; }
}
