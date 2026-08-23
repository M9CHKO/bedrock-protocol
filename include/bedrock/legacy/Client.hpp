#pragma once

#include <bedrock/legacy/Packet.hpp>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bedrock::legacy {

class Client {
public:
    using Handler = std::function<void(Packet&)>;

    void on(const std::string& name, Handler handler) {
        handlers_[name].push_back(std::move(handler));
    }

    void emit(Packet packet) {
        emitTo("packet", packet);
        emitTo(packet.name, packet);
    }

private:
    std::map<std::string, std::vector<Handler>> handlers_;

    void emitTo(const std::string& name, Packet& packet) {
        auto it = handlers_.find(name);
        if (it == handlers_.end()) return;
        for (auto& handler : it->second) handler(packet);
    }
};

} // namespace bedrock::legacy
