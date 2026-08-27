#pragma once

#include <bedrock/events/BedrockPacketEvent.hpp>
#include <bedrock/events/BedrockPacketEventAdapter.hpp>
#include <bedrock/protocol/GamePacket.hpp>
#include <bedrock/protodef/ProtoDefVariables.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {

using BedrockPacketEventHandler = std::function<void(const BedrockPacketEvent&)>;

class BedrockPacketEventDispatcher {
public:
    class EventBus {
    public:
        void onAny(BedrockPacketEventHandler handler) {
            anyHandlers_.push_back(std::move(handler));
        }

        void emit(const BedrockPacketEvent& event) const {
            for (const auto& handler : anyHandlers_) {
                handler(event);
            }
        }

    private:
        std::vector<BedrockPacketEventHandler> anyHandlers_;
    };

    explicit BedrockPacketEventDispatcher(
        std::string minecraftVersion,
        ProtoDefVariableStorePtr variables = {}
    ) : minecraftVersion_(std::move(minecraftVersion)),
        variables_(variables ? std::move(variables) : makeProtoDefVariableStore()) {}

    EventBus& events() {
        return events_;
    }

    const EventBus& events() const {
        return events_;
    }

    void on(const std::string& packetName, BedrockPacketEventHandler handler) {
        namedHandlers_[packetName].push_back(std::move(handler));
    }

    ProtoDefVariableStorePtr variableStore() const {
        return variables_;
    }

    void setVariable(std::string key, std::string value) {
        variables_->setVariable(std::move(key), std::move(value));
    }

    BedrockPacketEvent dispatch(const GamePacket& packet) {
        auto event = BedrockPacketEventAdapter::fromGamePacket(
            packet,
            minecraftVersion_,
            variables_
        );

        events_.emit(event);

        auto it = namedHandlers_.find(event.packetName);
        if (it != namedHandlers_.end()) {
            for (const auto& handler : it->second) {
                handler(event);
            }
        }

        return event;
    }

private:
    std::string minecraftVersion_;
    ProtoDefVariableStorePtr variables_;
    EventBus events_;
    std::unordered_map<std::string, std::vector<BedrockPacketEventHandler>> namedHandlers_;
};

} // namespace bedrock
