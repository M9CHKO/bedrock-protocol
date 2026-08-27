#pragma once

#include <bedrock/protodef/ProtoDefValue.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace bedrock {

struct ProtoDefField {
    std::string path;
    std::string type;
    std::string value;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::optional<ProtoDefValue> structuredValue;
};

}
