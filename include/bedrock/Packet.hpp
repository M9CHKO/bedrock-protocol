#pragma once

// Compatibility forwarding header. There is one canonical public Packet
// type, the same one exposed by <bedrock/bedrock.hpp>.
#include <bedrock/api/Client.hpp>

namespace bedrock {
using Packet = api::Packet;
}
