#pragma once

#include <bedrock/nbt/BedrockNbt.hpp>
#include <bedrock/protodef/ProtoDefReader.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>

#include <string_view>

namespace bedrock {

// Converts between prismarine-nbt's public shape
// { type, name?, value } and the native Bedrock NBT model.
NbtTagType protoDefNbtTagType(std::string_view name);
std::string_view protoDefNbtTagName(NbtTagType type);

NbtValue protoDefValueToNbtValue(const ProtoDefValue& value);
ProtoDefValue nbtValueToProtoDefValue(const NbtValue& value);

NbtDocument protoDefValueToNbtDocument(const ProtoDefValue& value);
ProtoDefValue nbtDocumentToProtoDefValue(const NbtDocument& document);

// Packet-level named NBT. This preserves the exact nbt/lnbt behavior used by
// bedrock-protocol and prismarine-nbt, including their root TAG_End handling.
void writeProtoDefNbt(
    ProtoDefWriter& writer,
    const ProtoDefValue& value,
    BedrockNbtEncoding encoding
);

ProtoDefValue readProtoDefNbt(
    ProtoDefReader& reader,
    BedrockNbtEncoding encoding
);

} // namespace bedrock
