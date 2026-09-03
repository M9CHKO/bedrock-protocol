#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bedrock {

struct ProtoDefValue {
    enum class Kind {
        Null,
        Bool,
        Int,
        UInt,
        Double,
        String,
        Bytes,
        Object,
        Array
    };

    Kind kind = Kind::Null;

    bool boolValue = false;
    int64_t intValue = 0;
    uint64_t uintValue = 0;
    double doubleValue = 0.0;
    std::string stringValue;
    std::vector<uint8_t> bytesValue;

    std::unordered_map<std::string, ProtoDefValue> objectValue;
    std::vector<ProtoDefValue> arrayValue;

    // Encoder-only navigation link used while a structured value is being
    // encoded. ProtoDef compareTo paths such as "../action" need access to
    // the containing object. Keeping that relationship as a non-owning link
    // avoids recursively copying the complete parent (which may itself own a
    // large array) into every array item.
    const ProtoDefValue* parentValue = nullptr;

    static ProtoDefValue null() {
        return {};
    }

    static ProtoDefValue boolean(bool v) {
        ProtoDefValue x;
        x.kind = Kind::Bool;
        x.boolValue = v;
        return x;
    }

    static ProtoDefValue integer(int64_t v) {
        ProtoDefValue x;
        x.kind = Kind::Int;
        x.intValue = v;
        return x;
    }

    static ProtoDefValue uinteger(uint64_t v) {
        ProtoDefValue x;
        x.kind = Kind::UInt;
        x.uintValue = v;
        return x;
    }

    static ProtoDefValue floating(double v) {
        ProtoDefValue x;
        x.kind = Kind::Double;
        x.doubleValue = v;
        return x;
    }

    static ProtoDefValue string(std::string v) {
        ProtoDefValue x;
        x.kind = Kind::String;
        x.stringValue = std::move(v);
        return x;
    }

    static ProtoDefValue bytes(std::vector<uint8_t> v) {
        ProtoDefValue x;
        x.kind = Kind::Bytes;
        x.bytesValue = std::move(v);
        return x;
    }

    static ProtoDefValue object(std::unordered_map<std::string, ProtoDefValue> v) {
        ProtoDefValue x;
        x.kind = Kind::Object;
        x.objectValue = std::move(v);
        return x;
    }

    static ProtoDefValue array(std::vector<ProtoDefValue> v) {
        ProtoDefValue x;
        x.kind = Kind::Array;
        x.arrayValue = std::move(v);
        return x;
    }

    const ProtoDefValue* get(const std::string& key) const {
        if (key == ".." && parentValue) {
            return parentValue;
        }
        auto it = objectValue.find(key);
        if (it == objectValue.end()) {
            return nullptr;
        }
        return &it->second;
    }
};

}
