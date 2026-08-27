#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

class SnappyCodecError : public std::runtime_error {
public:
    explicit SnappyCodecError(const std::string& message)
        : std::runtime_error(message) {}
};

// Raw Snappy block codec used by Bedrock's compression algorithm 1. This is
// the Snappy compressed format (length preamble plus literal/copy elements),
// not the optional .sz streaming/framing format.
class SnappyCodec {
public:
    static std::vector<uint8_t> compress(const std::vector<uint8_t>& input);
    static std::vector<uint8_t> decompress(const std::vector<uint8_t>& input);
};

} // namespace bedrock
