#pragma once

#include <bedrock/protodef/ProtoDefCompareExpression.hpp>
#include <bedrock/protodef/ProtoDefNbt.hpp>
#include <bedrock/protodef/ProtoDefValue.hpp>
#include <bedrock/protodef/ProtoDefVariables.hpp>
#include <bedrock/protodef/ProtoDefWriter.hpp>
#include <bedrock/generated/GeneratedProtocolTypes.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bedrock {

class ProtoDefEncoder {
public:
    using TypeResolver = std::function<std::optional<std::string>(const std::string&)>;

    ProtoDefEncoder() = default;

    explicit ProtoDefEncoder(TypeResolver resolver)
        : resolver_(std::move(resolver)) {}

    void setVariable(std::string key, std::string value) {
        variables_[std::move(key)] = std::move(value);
    }

    void setVariable(std::string key, const char* value) {
        setVariable(std::move(key), value ? std::string(value) : std::string());
    }

    void setVariable(std::string key, bool value) {
        setVariable(std::move(key), value ? "true" : "false");
    }

    template<std::integral T>
        requires (!std::same_as<T, bool>)
    void setVariable(std::string key, T value) {
        if constexpr (std::signed_integral<T>) {
            setVariable(std::move(key), std::to_string(static_cast<long long>(value)));
        } else {
            setVariable(std::move(key), std::to_string(static_cast<unsigned long long>(value)));
        }
    }

    template<std::floating_point T>
    void setVariable(std::string key, T value) {
        std::ostringstream out;
        out << std::setprecision(17) << value;
        setVariable(std::move(key), out.str());
    }

    void setVariables(const ProtoDefVariableMap& variables) {
        variables_ = variables;
    }

    std::optional<std::string> variable(const std::string& key) const {
        const auto found = variables_.find(key);
        return found == variables_.end()
            ? std::nullopt
            : std::optional<std::string>(found->second);
    }

    void encode(
        const std::string& typeJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        const std::string type = trim(typeJson);

        if (isJsonString(type)) {
            encodeTypeName(unquote(type), value, writer);
            return;
        }

        if (startsWith(type, "[\"container\"")) {
            encodeContainer(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"mapper\"")) {
            encodeMapper(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"switch\"")) {
            encodeSwitch(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"array\"")) {
            encodeArray(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"endOfArray\"")) {
            encodeEndOfArray(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"entityMetadataLoop\"")) {
            encodeEntityMetadataLoop(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"count\"")) {
            encodeCount(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"option\"")) {
            encodeOption(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"pstring\"")) {
            encodePString(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"buffer\"")) {
            encodeBuffer(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"encapsulated\"")) {
            encodeEncapsulated(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"bitflags\"")) {
            encodeBitflags(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"bitfield\"")) {
            encodeBitfield(type, value, writer);
            return;
        }

        if (startsWith(type, "[\"entityMetadataItem\"")) {
            encodeEntityMetadataItem(type, value, writer);
            return;
        }

        throw std::runtime_error("ProtoDefEncoder unsupported type json: " + type);
    }

private:


    void encodeEncapsulated(
        const std::string& encapsulatedJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        auto innerType = readJsonValueField(encapsulatedJson, "type");
        if (!innerType.has_value()) {
            throw std::runtime_error("encapsulated encode inner type not found");
        }

        ProtoDefWriter innerWriter;
        encode(*innerType, value, innerWriter);

        auto payload = innerWriter.take();

        auto lengthType =
            readJsonStringField(encapsulatedJson, "lengthType")
                .value_or("varint");

        writeCount(lengthType, payload.size(), writer);
        writer.bytes(payload);
    }


    void encodeBuffer(
        const std::string& bufferJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Bytes) {
            throw std::runtime_error("buffer encode expects bytes");
        }

        auto fixedCount = readJsonIntegerField(bufferJson, "count");
        auto countRef = readJsonStringField(bufferJson, "count");

        if (fixedCount.has_value()) {
            if (value.bytesValue.size() != *fixedCount) {
                throw std::runtime_error(
                    "buffer encode fixed count mismatch: expected " +
                    std::to_string(*fixedCount) + ", got " +
                    std::to_string(value.bytesValue.size())
                );
            }
        } else if (!countRef.has_value()) {
            auto countType =
                readJsonStringField(bufferJson, "countType")
                    .value_or("varint");

            writeCount(
                countType,
                value.bytesValue.size(),
                writer
            );
        }

        writer.bytes(
            value.bytesValue.data(),
            value.bytesValue.size()
        );
    }

    void encodePString(
        const std::string& stringJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        const std::string& text = asString(value);
        auto countType = readJsonStringField(stringJson, "countType").value_or("varint");
        writeCount(countType, text.size(), writer);
        writer.bytes(
            reinterpret_cast<const uint8_t*>(text.data()),
            text.size()
        );
    }


    void encodeOption(
        const std::string& optionJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind == ProtoDefValue::Kind::Null) {
            writer.u8(0);
            return;
        }

        writer.u8(1);

        auto innerType = readSecondElement(optionJson);
        if (!innerType.has_value()) {
            throw std::runtime_error("option encode inner type not found");
        }

        encode(*innerType, value, writer);
    }


    void encodeCount(
        const std::string& countJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        auto countFor = readJsonStringField(countJson, "countFor");
        auto type = readJsonValueField(countJson, "type");
        if (!type.has_value()) {
            throw std::runtime_error("count encode type not found");
        }

        std::size_t count = 0;
        if (countFor.has_value()) {
            const ProtoDefValue* target = getPath(value, *countFor);
            if (!target) {
                throw std::runtime_error("count encode missing countFor field: " + *countFor);
            }
            count = valueLength(*target);
        } else {
            count = static_cast<std::size_t>(asUInt(value));
        }

        encode(*type, ProtoDefValue::uinteger(count), writer);
    }


    void encodeArray(
        const std::string& arrayJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Array) {
            throw std::runtime_error("array encode expects array");
        }

        auto itemType = readJsonValueField(arrayJson, "type");
        if (!itemType.has_value()) {
            throw std::runtime_error("array encode item type not found");
        }

        // count:"field" or count:123 means the length is already known.
        auto fixedCount = readJsonIntegerField(arrayJson, "count");
        auto countRef = readJsonStringField(arrayJson, "count");
        if (fixedCount.has_value()) {
            if (value.arrayValue.size() != *fixedCount) {
                throw std::runtime_error(
                    "array encode fixed count mismatch: expected " +
                    std::to_string(*fixedCount) + ", got " +
                    std::to_string(value.arrayValue.size())
                );
            }
        } else if (!countRef.has_value()) {
            auto countType = readJsonStringField(arrayJson, "countType").value_or("varint");
            writeCount(countType, value.arrayValue.size(), writer);
        }

        for (const auto& item : value.arrayValue) {
            encode(*itemType, item, writer);
        }
    }

    void encodeEndOfArray(
        const std::string& arrayJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Array) {
            throw std::runtime_error("endOfArray encode expects array");
        }

        auto itemType = readJsonValueField(arrayJson, "type");
        if (!itemType.has_value()) {
            throw std::runtime_error("endOfArray encode item type not found");
        }

        for (const auto& item : value.arrayValue) {
            encode(*itemType, item, writer);
        }
    }

    void encodeEntityMetadataLoop(
        const std::string& metadataJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Array) {
            throw std::runtime_error("entityMetadataLoop encode expects array");
        }

        auto itemType = readJsonValueField(metadataJson, "type");
        auto endVal = readJsonIntegerField(metadataJson, "endVal").value_or(0x7f);
        if (!itemType.has_value()) {
            throw std::runtime_error("entityMetadataLoop encode item type not found");
        }

        for (const auto& item : value.arrayValue) {
            encode(*itemType, item, writer);
        }
        writer.u8(static_cast<uint8_t>(endVal));
    }

    void writeCount(
        const std::string& countType,
        std::size_t count,
        ProtoDefWriter& writer
    ) const {
        if (countType == "u8" || countType == "lu8" || countType == "li8") {
            writer.u8(static_cast<uint8_t>(count));
            return;
        }

        if (countType == "u16") {
            writer.u16be(static_cast<uint16_t>(count));
            return;
        }

        if (countType == "i16") {
            writer.u16be(static_cast<uint16_t>(count));
            return;
        }

        if (countType == "lu16" || countType == "li16") {
            writer.u16le(static_cast<uint16_t>(count));
            return;
        }

        if (countType == "u32") {
            writer.u32be(static_cast<uint32_t>(count));
            return;
        }

        if (countType == "i32") {
            writer.u32be(static_cast<uint32_t>(count));
            return;
        }

        if (countType == "lu32" || countType == "li32") {
            writer.u32le(static_cast<uint32_t>(count));
            return;
        }

        if (countType == "varint" || countType == "varuint") {
            writer.varuint32(static_cast<uint32_t>(count));
            return;
        }

        if (countType == "zigzag32") {
            writer.zigzag32(static_cast<int32_t>(count));
            return;
        }

        if (countType == "zigzag64") {
            writer.zigzag64(static_cast<int64_t>(count));
            return;
        }

        if (countType == "varint64" || countType == "varuint64") {
            writer.varuint64(static_cast<uint64_t>(count));
            return;
        }

        throw std::runtime_error("array encode unsupported countType: " + countType);
    }

    static std::size_t valueLength(const ProtoDefValue& value) {
        if (value.kind == ProtoDefValue::Kind::Array) return value.arrayValue.size();
        if (value.kind == ProtoDefValue::Kind::Bytes) return value.bytesValue.size();
        if (value.kind == ProtoDefValue::Kind::String) return value.stringValue.size();
        if (value.kind == ProtoDefValue::Kind::Object) return value.objectValue.size();
        throw std::runtime_error("count encode target has no length");
    }


    void encodeSwitch(
        const std::string& switchJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Object) {
            throw std::runtime_error("switch encode expects object context");
        }

        std::string compareValue;

        auto compareToValue = readJsonStringField(switchJson, "compareToValue");
        if (compareToValue.has_value()) {
            compareValue = *compareToValue;
        } else {
            auto compareTo = readJsonStringField(switchJson, "compareTo");
            if (!compareTo.has_value()) {
                throw std::runtime_error("switch encode compareTo not found");
            }

            auto resolvedCompare = resolveCompareValue(value, *compareTo);
            if (!resolvedCompare.has_value()) {
                throw std::runtime_error("switch encode missing compare field: " + *compareTo);
            }

            compareValue = *resolvedCompare;
        }

        auto branch = findSwitchBranchType(switchJson, compareValue);
        if (!branch.has_value()) {
            branch = readJsonValueField(switchJson, "default");
        }

        if (!branch.has_value()) {
            return;
        }

        if (const ProtoDefValue* directValue = value.get("$value")) {
            encode(*branch, *directValue, writer);
        } else {
            encode(*branch, value, writer);
        }
    }


    void encodeMapper(
        const std::string& mapperJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        auto baseType = readJsonStringField(mapperJson, "type");
        if (!baseType.has_value()) {
            throw std::runtime_error("mapper encode base type not found");
        }

        uint64_t numeric = 0;

        if (value.kind == ProtoDefValue::Kind::String) {
            auto found = findMapperNumericByName(mapperJson, value.stringValue);
            if (!found.has_value()) {
                throw std::runtime_error("mapper encode unknown mapped value: " + value.stringValue);
            }
            numeric = *found;
        } else {
            numeric = asUInt(value);
        }

        encodeTypeName(*baseType, ProtoDefValue::uinteger(numeric), writer);
    }


    void encodeContainer(
        const std::string& containerJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Object) {
            throw std::runtime_error("container encode expects object");
        }

        auto fields = findSecondArray(containerJson);
        if (!fields.has_value()) {
            throw std::runtime_error("container fields array not found");
        }

        std::size_t pos = 0;
        while (true) {
            auto objStart = fields->find('{', pos);
            if (objStart == std::string::npos) break;

            auto objEnd = findMatching(*fields, objStart, '{', '}');
            if (objEnd == std::string::npos) {
                throw std::runtime_error("container field object not closed");
            }

            std::string fieldObj = fields->substr(objStart, objEnd - objStart + 1);

            auto name = readJsonStringField(fieldObj, "name");
            auto type = readJsonValueField(fieldObj, "type");
            bool anon = readJsonBoolField(fieldObj, "anon").value_or(false);

            if (!type.has_value()) {
                pos = objEnd + 1;
                continue;
            }

            try {
                if (anon) {
                    encode(*type, value, writer);
                } else {
                    if (!name.has_value()) {
                        throw std::runtime_error("container field missing name");
                    }

                    std::string normalizedType = trim(*type);

                    if (
                        startsWith(normalizedType, "[\"switch\"") ||
                        startsWith(normalizedType, "[\"entityMetadataItem\"")
                    ) {
                        ProtoDefValue switchContext = value;

                        if (const ProtoDefValue* child = value.get(*name)) {
                            switchContext.objectValue["$value"] = withParent(*child, value);
                        }

                        encode(*type, switchContext, writer);
                    } else {
                        const ProtoDefValue* child = value.get(*name);
                        if (!child) {
                            throw std::runtime_error("container missing field: " + *name);
                        }

                        encode(*type, withParent(*child, value), writer);
                    }
                }
            } catch (const std::exception& error) {
                auto fieldLabel = anon
                    ? std::string("<anonymous ") + trim(*type).substr(0, 96) + ">"
                    : name.value_or("<unnamed>");
                throw std::runtime_error(
                    "at container field " + fieldLabel + ": " + error.what()
                );
            }

            pos = objEnd + 1;
        }
    }

    void encodeTypeName(
        const std::string& typeName,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (typeName == "void") {
            return;
        }

        if (typeName == "string") {
            writer.string(asString(value));
            return;
        }

        if (typeName == "ShortString") {
            writer.shortString(asString(value));
            return;
        }

        if (typeName == "lstring") {
            const std::string& text = asString(value);
            writer.u32le(static_cast<uint32_t>(text.size()));
            writer.bytes(
                reinterpret_cast<const uint8_t*>(text.data()),
                text.size()
            );
            return;
        }

        if (typeName == "uuid") {
            writeUuid(asString(value), writer);
            return;
        }

        if (typeName == "ipAddress") {
            writeIpAddress(asString(value), writer);
            return;
        }

        if (typeName == "restBuffer" || typeName == "MapInfo") {
            if (value.kind != ProtoDefValue::Kind::Bytes) {
                throw std::runtime_error(typeName + " encode expects bytes");
            }
            writer.bytes(value.bytesValue);
            return;
        }

        if (typeName == "byterot") {
            const double rotation = asDouble(value);
            writer.u8(static_cast<uint8_t>(rotation / (360.0 / 256.0)));
            return;
        }

        if (typeName == "nbtLoop") {
            if (value.kind != ProtoDefValue::Kind::Array) {
                throw std::runtime_error("nbtLoop encode expects array");
            }
            for (const auto& item : value.arrayValue) {
                if (item.kind != ProtoDefValue::Kind::Bytes &&
                    protoDefValueToNbtDocument(item).root.type == NbtTagType::End) {
                    throw std::runtime_error("nbtLoop cannot contain TAG_End values");
                }
                writeProtoDefNbt(writer, item, BedrockNbtEncoding::LittleVarInt);
            }
            writer.u8(0);
            return;
        }

        if (typeName == "enum_size_based_on_values_len") {
            return;
        }

        if (typeName == "native" || typeName == "nbt") {
            writeProtoDefNbt(writer, value, BedrockNbtEncoding::LittleVarInt);
            return;
        }

        if (typeName == "lnbt") {
            writeProtoDefNbt(writer, value, BedrockNbtEncoding::LittleEndian);
            return;
        }

        if (typeName == "bool") {
            writer.boolValue(asBool(value));
            return;
        }

        if (typeName == "u8" || typeName == "lu8" || typeName == "li8") {
            writer.u8(static_cast<uint8_t>(asUInt(value)));
            return;
        }

        if (typeName == "i8" || typeName == "byte") {
            writer.u8(static_cast<uint8_t>(asInt(value)));
            return;
        }

        if (typeName == "u16") {
            writer.u16be(static_cast<uint16_t>(asUInt(value)));
            return;
        }

        if (typeName == "lu16") {
            writer.u16le(static_cast<uint16_t>(asUInt(value)));
            return;
        }

        if (typeName == "i16") {
            writer.u16be(static_cast<uint16_t>(asInt(value)));
            return;
        }

        if (typeName == "li16") {
            writer.u16le(static_cast<uint16_t>(asInt(value)));
            return;
        }

        if (typeName == "u32") {
            writer.u32be(static_cast<uint32_t>(asUInt(value)));
            return;
        }

        if (typeName == "lu32") {
            writer.u32le(static_cast<uint32_t>(asUInt(value)));
            return;
        }

        if (typeName == "u64") {
            writer.u64be(static_cast<uint64_t>(asUInt(value)));
            return;
        }

        if (typeName == "lu64") {
            writer.u64le(static_cast<uint64_t>(asUInt(value)));
            return;
        }

        if (typeName == "i32") {
            writer.u32be(static_cast<uint32_t>(asInt(value)));
            return;
        }

        if (typeName == "li32") {
            writer.u32le(static_cast<uint32_t>(asInt(value)));
            return;
        }

        if (typeName == "i64") {
            writer.u64be(static_cast<uint64_t>(asInt(value)));
            return;
        }

        if (typeName == "li64") {
            writer.u64le(static_cast<uint64_t>(asInt(value)));
            return;
        }

        if (typeName == "varint" || typeName == "varuint") {
            writer.varuint32(static_cast<uint32_t>(asUInt(value)));
            return;
        }

        if (
            typeName == "varuint64" ||
            typeName == "varint64" ||
            typeName == "varlong" ||
            typeName == "entity_runtime_id" ||
            typeName == "actor_runtime_id" ||
            typeName == "runtime_entity_id"
        ) {
            writer.varuint64(asUInt(value));
            return;
        }

        if (typeName == "zigzag32") {
            writer.zigzag32(static_cast<int32_t>(asInt(value)));
            return;
        }

        if (typeName == "zigzag64") {
            writer.zigzag64(static_cast<int64_t>(asInt(value)));
            return;
        }

        if (typeName == "varint128") {
            writer.varuint128(static_cast<unsigned __int128>(asUInt(value)));
            return;
        }

        if (typeName == "f32") {
            writer.f32be(static_cast<float>(asDouble(value)));
            return;
        }

        if (typeName == "lf32") {
            writer.f32le(static_cast<float>(asDouble(value)));
            return;
        }

        if (typeName == "f64") {
            writer.f64be(static_cast<double>(asDouble(value)));
            return;
        }

        if (typeName == "lf64") {
            writer.f64le(static_cast<double>(asDouble(value)));
            return;
        }

        if (resolver_) {
            auto resolved = resolver_(typeName);
            if (resolved.has_value()) {
                encode(*resolved, value, writer);
                return;
            }
        }

        auto generated = bedrock::generatedProtocolTypeJson(typeName);
        if (generated.has_value()) {
            encode(*generated, value, writer);
            return;
        }

        throw std::runtime_error("ProtoDefEncoder unknown primitive type: " + typeName);
    }


    static bool asBool(const ProtoDefValue& v) {
        if (v.kind == ProtoDefValue::Kind::Bool) return v.boolValue;
        if (v.kind == ProtoDefValue::Kind::Int) return v.intValue != 0;
        if (v.kind == ProtoDefValue::Kind::UInt) return v.uintValue != 0;
        if (v.kind == ProtoDefValue::Kind::Double) return v.doubleValue != 0.0;
        if (v.kind == ProtoDefValue::Kind::String) return v.stringValue == "true" || v.stringValue == "1";
        throw std::runtime_error("expected bool-compatible value");
    }

    static int64_t asInt(const ProtoDefValue& v) {
        if (v.kind == ProtoDefValue::Kind::Int) return v.intValue;
        if (v.kind == ProtoDefValue::Kind::UInt) return static_cast<int64_t>(v.uintValue);
        if (v.kind == ProtoDefValue::Kind::Bool) return v.boolValue ? 1 : 0;
        if (v.kind == ProtoDefValue::Kind::Double) return static_cast<int64_t>(v.doubleValue);
        throw std::runtime_error("expected int-compatible value");
    }

    static uint64_t asUInt(const ProtoDefValue& v) {
        if (v.kind == ProtoDefValue::Kind::UInt) return v.uintValue;
        if (v.kind == ProtoDefValue::Kind::Int) return static_cast<uint64_t>(v.intValue);
        if (v.kind == ProtoDefValue::Kind::Bool) return v.boolValue ? 1 : 0;
        if (v.kind == ProtoDefValue::Kind::Double) return static_cast<uint64_t>(v.doubleValue);
        throw std::runtime_error("expected uint-compatible value");
    }

    static unsigned __int128 asUInt128(const ProtoDefValue& v) {
        if (v.kind == ProtoDefValue::Kind::String) return parseUint128(v.stringValue);
        return static_cast<unsigned __int128>(asUInt(v));
    }

    static double asDouble(const ProtoDefValue& v) {
        if (v.kind == ProtoDefValue::Kind::Double) return v.doubleValue;
        if (v.kind == ProtoDefValue::Kind::Int) return static_cast<double>(v.intValue);
        if (v.kind == ProtoDefValue::Kind::UInt) return static_cast<double>(v.uintValue);
        if (v.kind == ProtoDefValue::Kind::Bool) return v.boolValue ? 1.0 : 0.0;
        throw std::runtime_error("expected numeric value");
    }

    static const std::string& asString(const ProtoDefValue& v) {
        if (v.kind != ProtoDefValue::Kind::String) {
            throw std::runtime_error("expected string value");
        }
        return v.stringValue;
    }

    static std::string valueToSwitchKey(const ProtoDefValue& value) {
        if (value.kind == ProtoDefValue::Kind::String) {
            return value.stringValue;
        }

        if (value.kind == ProtoDefValue::Kind::UInt) {
            return std::to_string(value.uintValue);
        }

        if (value.kind == ProtoDefValue::Kind::Int) {
            return std::to_string(value.intValue);
        }

        if (value.kind == ProtoDefValue::Kind::Bool) {
            return value.boolValue ? "true" : "false";
        }

        throw std::runtime_error("switch compare value unsupported kind");
    }

    static void writeUuid(const std::string& value, ProtoDefWriter& writer) {
        auto hexValue = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };

        std::string hex;
        hex.reserve(32);
        for (char c : value) {
            if (c == '-') continue;
            hex.push_back(c);
        }

        if (hex.size() != 32) {
            throw std::runtime_error("uuid encode expects 32 hex digits");
        }

        for (std::size_t i = 0; i < hex.size(); i += 2) {
            int hi = hexValue(hex[i]);
            int lo = hexValue(hex[i + 1]);
            if (hi < 0 || lo < 0) {
                throw std::runtime_error("uuid encode invalid hex");
            }
            writer.u8(static_cast<uint8_t>((hi << 4) | lo));
        }
    }

    static void writeIpAddress(const std::string& value, ProtoDefWriter& writer) {
        std::size_t start = 0;
        for (int i = 0; i < 4; ++i) {
            auto dot = value.find('.', start);
            std::string part = dot == std::string::npos
                ? value.substr(start)
                : value.substr(start, dot - start);
            if (part.empty()) {
                throw std::runtime_error("ipAddress encode invalid address");
            }
            int octet = std::stoi(part);
            if (octet < 0 || octet > 255) {
                throw std::runtime_error("ipAddress encode octet out of range");
            }
            writer.u8(static_cast<uint8_t>(octet));
            if (dot == std::string::npos) {
                if (i != 3) throw std::runtime_error("ipAddress encode expects 4 octets");
                start = value.size();
            } else {
                start = dot + 1;
            }
        }
        if (start < value.size()) {
            throw std::runtime_error("ipAddress encode expects 4 octets");
        }
    }

    void encodeBitflags(
        const std::string& bitflagsJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        auto baseType = readJsonStringField(bitflagsJson, "type").value_or("varint");

        unsigned __int128 mask = 0;
        if (value.kind == ProtoDefValue::Kind::Object) {
            if (const ProtoDefValue* raw = value.get("_value")) {
                mask = asUInt128(*raw);
            }
        }

        if (
            value.kind == ProtoDefValue::Kind::UInt ||
            value.kind == ProtoDefValue::Kind::Int ||
            value.kind == ProtoDefValue::Kind::Bool ||
            value.kind == ProtoDefValue::Kind::Double ||
            value.kind == ProtoDefValue::Kind::String
        ) {
            mask = asUInt128(value);
        } else if (value.kind == ProtoDefValue::Kind::Array) {
            auto flags = readBitflagValues128(bitflagsJson);
            for (const auto& item : value.arrayValue) {
                const std::string& name = asString(item);
                auto it = flags.find(name);
                if (it == flags.end()) {
                    throw std::runtime_error("unknown bitflag: " + name);
                }
                mask |= it->second;
            }
        } else if (value.kind == ProtoDefValue::Kind::Object) {
            auto flags = readBitflagValues128(bitflagsJson);
            // ProtoDef treats _value as the preserved raw mask and only ORs
            // named flags whose value is truthy. False flags and unrelated
            // object properties must not clear overlapping or unknown bits.
            for (const auto& [name, bit] : flags) {
                auto enabled = value.objectValue.find(name);
                if (enabled != value.objectValue.end() && asBool(enabled->second)) {
                    mask |= bit;
                }
            }
        } else {
            throw std::runtime_error("bitflags encode expects number, array, or object");
        }

        if (baseType == "varint128") {
            writer.varuint128(mask);
            return;
        }

        if (mask > static_cast<unsigned __int128>(UINT64_MAX)) {
            throw std::runtime_error("bitflags mask does not fit " + baseType);
        }

        encodeTypeName(baseType, ProtoDefValue::uinteger(static_cast<uint64_t>(mask)), writer);
    }

    struct BitfieldPart {
        std::string name;
        int size = 0;
        bool signedValue = false;
    };

    void encodeBitfield(
        const std::string& bitfieldJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Object) {
            throw std::runtime_error("bitfield encode expects object");
        }

        auto parts = readBitfieldParts(bitfieldJson);
        uint32_t toWrite = 0;
        int bits = 0;

        for (const auto& part : parts) {
            const ProtoDefValue* field = value.get(part.name);
            if (!field) {
                throw std::runtime_error("bitfield missing field: " + part.name);
            }

            int64_t raw = asInt(*field);
            if (part.size <= 0 || part.size > 31) {
                throw std::runtime_error("bitfield unsupported field size: " + std::to_string(part.size));
            }

            const int64_t minValue = part.signedValue ? -(1LL << (part.size - 1)) : 0;
            const int64_t maxValue = part.signedValue ? ((1LL << (part.size - 1)) - 1) : ((1LL << part.size) - 1);
            if (raw < minValue || raw > maxValue) {
                throw std::runtime_error("bitfield field out of range: " + part.name);
            }

            uint32_t val = static_cast<uint32_t>(raw);
            if (part.signedValue && raw < 0) {
                val = static_cast<uint32_t>((1LL << part.size) + raw);
            }

            int size = part.size;
            while (size > 0) {
                int writeBits = std::min(8 - bits, size);
                uint32_t mask = (1u << writeBits) - 1u;
                toWrite = (toWrite << writeBits) | ((val >> (size - writeBits)) & mask);
                size -= writeBits;
                bits += writeBits;

                if (bits == 8) {
                    writer.u8(static_cast<uint8_t>(toWrite));
                    bits = 0;
                    toWrite = 0;
                }
            }
        }

        if (bits != 0) {
            writer.u8(static_cast<uint8_t>(toWrite << (8 - bits)));
        }
    }

    void encodeEntityMetadataItem(
        const std::string& metadataItemJson,
        const ProtoDefValue& value,
        ProtoDefWriter& writer
    ) const {
        if (value.kind != ProtoDefValue::Kind::Object) {
            throw std::runtime_error("entityMetadataItem encode expects object context");
        }

        auto compareTo = readJsonStringField(metadataItemJson, "compareTo").value_or("type");
        auto compareValue = resolveCompareValue(value, compareTo);
        if (!compareValue.has_value()) {
            throw std::runtime_error("entityMetadataItem missing compare field: " + compareTo);
        }

        auto switchJson = resolver_ ? resolver_("entityMetadataItem") : std::nullopt;
        if (!switchJson.has_value()) {
            switchJson = bedrock::generatedProtocolTypeJson("entityMetadataItem");
        }
        if (!switchJson.has_value()) {
            throw std::runtime_error("entityMetadataItem schema not found");
        }

        auto branch = findSwitchBranchType(*switchJson, *compareValue);
        if (!branch.has_value()) {
            branch = readJsonValueField(*switchJson, "default");
        }
        if (!branch.has_value()) {
            throw std::runtime_error("entityMetadataItem no branch for: " + *compareValue);
        }

        if (const ProtoDefValue* directValue = value.get("$value")) {
            encode(*branch, *directValue, writer);
        } else {
            encode(*branch, value, writer);
        }
    }

    static std::vector<BitfieldPart> readBitfieldParts(const std::string& bitfieldJson) {
        auto fields = readSecondElement(bitfieldJson);
        if (!fields.has_value()) {
            throw std::runtime_error("bitfield fields array not found");
        }

        std::vector<BitfieldPart> out;
        std::size_t pos = 0;
        while (true) {
            auto objStart = fields->find('{', pos);
            if (objStart == std::string::npos) break;

            auto objEnd = findMatching(*fields, objStart, '{', '}');
            if (objEnd == std::string::npos) {
                throw std::runtime_error("bitfield field object not closed");
            }

            std::string fieldObj = fields->substr(objStart, objEnd - objStart + 1);
            auto name = readJsonStringField(fieldObj, "name");
            auto size = readJsonIntegerField(fieldObj, "size");
            auto signedValue = readJsonBoolField(fieldObj, "signed").value_or(false);
            if (!name.has_value() || !size.has_value()) {
                throw std::runtime_error("bitfield field missing name or size");
            }

            out.push_back(BitfieldPart{*name, static_cast<int>(*size), signedValue});
            pos = objEnd + 1;
        }

        return out;
    }

    static std::unordered_map<std::string, unsigned __int128> readBitflagValues128(const std::string& bitflagsJson) {
        std::unordered_map<std::string, unsigned __int128> out;
        auto flagsValue = readJsonValueField(bitflagsJson, "flags");
        if (!flagsValue.has_value()) return out;

        const std::string flags = trim(*flagsValue);
        if (flags.empty()) return out;

        if (flags[0] == '[') {
            std::size_t pos = 0;
            unsigned __int128 bit = 1;
            while (true) {
                auto q1 = flags.find('"', pos);
                if (q1 == std::string::npos) break;
                auto q2 = flags.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                out[flags.substr(q1 + 1, q2 - q1 - 1)] = bit;
                bit <<= 1;
                pos = q2 + 1;
            }
            return out;
        }

        if (flags[0] == '{') {
            std::size_t pos = 0;
            while (true) {
                auto q1 = flags.find('"', pos);
                if (q1 == std::string::npos) break;
                auto q2 = flags.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string name = flags.substr(q1 + 1, q2 - q1 - 1);

                auto colon = flags.find(':', q2 + 1);
                if (colon == std::string::npos) break;
                std::size_t n = colon + 1;
                while (n < flags.size() && std::isspace(static_cast<unsigned char>(flags[n]))) ++n;
                std::size_t e = n;
                while (e < flags.size() && std::isdigit(static_cast<unsigned char>(flags[e]))) ++e;
                out[name] = parseUint128(flags.substr(n, e - n));
                pos = e;
            }
        }

        return out;
    }

    static std::unordered_map<std::string, uint64_t> readBitflagValues(const std::string& bitflagsJson) {
        std::unordered_map<std::string, uint64_t> out;
        auto flagsValue = readJsonValueField(bitflagsJson, "flags");
        if (!flagsValue.has_value()) return out;

        const std::string flags = trim(*flagsValue);
        if (flags.empty()) return out;

        if (flags[0] == '[') {
            std::size_t pos = 0;
            uint64_t bit = 1;
            while (true) {
                auto q1 = flags.find('"', pos);
                if (q1 == std::string::npos) break;
                auto q2 = flags.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                out[flags.substr(q1 + 1, q2 - q1 - 1)] = bit;
                bit <<= 1;
                pos = q2 + 1;
            }
            return out;
        }

        if (flags[0] == '{') {
            std::size_t pos = 0;
            while (true) {
                auto q1 = flags.find('"', pos);
                if (q1 == std::string::npos) break;
                auto q2 = flags.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                std::string name = flags.substr(q1 + 1, q2 - q1 - 1);

                auto colon = flags.find(':', q2 + 1);
                if (colon == std::string::npos) break;
                std::size_t n = colon + 1;
                while (n < flags.size() && std::isspace(static_cast<unsigned char>(flags[n]))) ++n;
                std::size_t e = n;
                while (e < flags.size() && std::isdigit(static_cast<unsigned char>(flags[e]))) ++e;
                out[name] = static_cast<uint64_t>(std::stoull(flags.substr(n, e - n)));
                pos = e;
            }
        }

        return out;
    }

    static unsigned __int128 parseUint128(const std::string& text) {
        unsigned __int128 out = 0;
        for (char c : text) {
            if (c < '0' || c > '9') continue;
            out = out * 10 + static_cast<unsigned __int128>(c - '0');
        }
        return out;
    }

    static ProtoDefValue withParent(const ProtoDefValue& value, const ProtoDefValue& parent) {
        if (value.kind == ProtoDefValue::Kind::Object) {
            ProtoDefValue copy = value;
            copy.objectValue[".."] = parent;
            return copy;
        }

        if (value.kind == ProtoDefValue::Kind::Array) {
            ProtoDefValue copy = value;
            for (auto& item : copy.arrayValue) {
                item = withParent(item, parent);
            }
            return copy;
        }

        return value;
    }

    static std::optional<std::string> resolveCompareValue(
        const ProtoDefValue& object,
        const std::string& expression
    ) {
        const bool booleanExpression =
            detail::hasProtoDefLogicalOr(expression);
        return detail::evaluateProtoDefCompareExpression(
            expression,
            [&](std::string_view atom)
                -> std::optional<detail::ProtoDefCompareAtom> {
                const ProtoDefValue* value = getPath(
                    object,
                    std::string(atom)
                );
                if (!value) return std::nullopt;
                return detail::ProtoDefCompareAtom {
                    booleanExpression
                        ? std::string()
                        : valueToSwitchKey(*value),
                    booleanExpression ? asBool(*value) : false
                };
            }
        );
    }

    static const ProtoDefValue* getPath(const ProtoDefValue& object, const std::string& path) {
        const ProtoDefValue* current = &object;
        std::string normalized = path;
        std::replace(normalized.begin(), normalized.end(), '.', '/');

        std::size_t start = 0;
        if (!normalized.empty() && normalized[0] == '/') {
            while (current && current->get("..")) {
                current = current->get("..");
            }
            start = 1;
        }

        while (current && start <= normalized.size()) {
            auto slash = normalized.find('/', start);
            std::string part = slash == std::string::npos
                ? normalized.substr(start)
                : normalized.substr(start, slash - start);

            if (!part.empty()) {
                current = current->get(part);
            }

            if (slash == std::string::npos) return current;
            start = slash + 1;
        }
        return current;
    }

    std::optional<std::string> findSwitchBranchType(
        const std::string& switchJson,
        const std::string& key
    ) const {
        std::string fieldsNeedle = "\"fields\"";
        auto f = switchJson.find(fieldsNeedle);
        if (f == std::string::npos) return std::nullopt;

        auto objStart = switchJson.find('{', f + fieldsNeedle.size());
        if (objStart == std::string::npos) return std::nullopt;

        auto objEnd = findMatching(switchJson, objStart, '{', '}');
        if (objEnd == std::string::npos) return std::nullopt;

        std::string obj = switchJson.substr(objStart, objEnd - objStart + 1);

        // ProtoDef variables are referenced by switch field names prefixed
        // with '/'. Their primitive value becomes the actual switch key.
        for (const auto& [name, value] : variables_) {
            if (!switchValuesEqual(key, value)) continue;
            if (auto branch = readSwitchObjectField(obj, "/" + name)) {
                return branch;
            }
        }

        for (const auto& candidate : switchLookupKeys(key)) {
            if (auto branch = readSwitchObjectField(obj, candidate)) return branch;
        }

        return std::nullopt;
    }

    static std::vector<std::string> switchLookupKeys(const std::string& value) {
        std::vector<std::string> keys;
        keys.push_back(value);

        auto slash = value.find('/');
        if (slash != std::string::npos && slash + 1 < value.size()) {
            keys.push_back(value.substr(slash + 1));
        }

        keys.push_back("/" + value);

        if (slash != std::string::npos && slash + 1 < value.size()) {
            keys.push_back("/" + value.substr(slash + 1));
        }

        return keys;
    }

    static bool switchValuesEqual(
        const std::string& lhs,
        const std::string& rhs
    ) {
        const auto lhsKeys = switchLookupKeys(lhs);
        const auto rhsKeys = switchLookupKeys(rhs);
        return std::any_of(lhsKeys.begin(), lhsKeys.end(), [&](const auto& left) {
            return std::find(rhsKeys.begin(), rhsKeys.end(), left) != rhsKeys.end();
        });
    }

    static std::optional<std::string> readSwitchObjectField(
        const std::string& objectJson,
        const std::string& key
    ) {
        const std::string quoted = "\"" + key + "\"";
        const auto k = objectJson.find(quoted);
        if (k == std::string::npos) return std::nullopt;

        const auto colon = objectJson.find(':', k + quoted.size());
        if (colon == std::string::npos) return std::nullopt;

        std::size_t start = colon + 1;
        while (start < objectJson.size() &&
               std::isspace(static_cast<unsigned char>(objectJson[start]))) {
            ++start;
        }
        if (start >= objectJson.size()) return std::nullopt;

        if (objectJson[start] == '"') {
            const auto end = objectJson.find('"', start + 1);
            if (end != std::string::npos) {
                return objectJson.substr(start, end - start + 1);
            }
        } else if (objectJson[start] == '[') {
            const auto end = findMatching(objectJson, start, '[', ']');
            if (end != std::string::npos) {
                return objectJson.substr(start, end - start + 1);
            }
        } else if (objectJson[start] == '{') {
            const auto end = findMatching(objectJson, start, '{', '}');
            if (end != std::string::npos) {
                return objectJson.substr(start, end - start + 1);
            }
        }
        return std::nullopt;
    }


    static std::optional<std::string> readSecondElement(const std::string& json) {
        auto firstComma = json.find(',');
        if (firstComma == std::string::npos) return std::nullopt;

        std::size_t a = firstComma + 1;
        while (a < json.size() && std::isspace(static_cast<unsigned char>(json[a]))) {
            ++a;
        }

        if (a >= json.size()) return std::nullopt;

        if (json[a] == '"') {
            auto b = json.find('"', a + 1);
            if (b == std::string::npos) return std::nullopt;
            return json.substr(a, b - a + 1);
        }

        if (json[a] == '[') {
            auto b = findMatching(json, a, '[', ']');
            if (b == std::string::npos) return std::nullopt;
            return json.substr(a, b - a + 1);
        }

        if (json[a] == '{') {
            auto b = findMatching(json, a, '{', '}');
            if (b == std::string::npos) return std::nullopt;
            return json.substr(a, b - a + 1);
        }

        return std::nullopt;
    }


    static std::optional<uint64_t> findMapperNumericByName(
        const std::string& mapperJson,
        const std::string& mappedName
    ) {
        std::string mappingsNeedle = "\"mappings\"";
        auto m = mapperJson.find(mappingsNeedle);
        if (m == std::string::npos) return std::nullopt;

        auto objStart = mapperJson.find('{', m + mappingsNeedle.size());
        if (objStart == std::string::npos) return std::nullopt;

        auto objEnd = findMatching(mapperJson, objStart, '{', '}');
        if (objEnd == std::string::npos) return std::nullopt;

        std::string obj = mapperJson.substr(objStart, objEnd - objStart + 1);

        std::size_t pos = 0;
        while (true) {
            auto k1 = obj.find('"', pos);
            if (k1 == std::string::npos) break;

            auto k2 = obj.find('"', k1 + 1);
            if (k2 == std::string::npos) break;

            std::string key = obj.substr(k1 + 1, k2 - k1 - 1);

            auto colon = obj.find(':', k2 + 1);
            if (colon == std::string::npos) break;

            auto v1 = obj.find('"', colon + 1);
            if (v1 == std::string::npos) break;

            auto v2 = obj.find('"', v1 + 1);
            if (v2 == std::string::npos) break;

            std::string val = obj.substr(v1 + 1, v2 - v1 - 1);

            if (val == mappedName) {
                return static_cast<uint64_t>(std::stoull(key));
            }

            pos = v2 + 1;
        }

        return std::nullopt;
    }


    static std::optional<std::string> findSecondArray(const std::string& json) {
        auto first = json.find('[');
        if (first == std::string::npos) return std::nullopt;

        auto second = json.find('[', first + 1);
        if (second == std::string::npos) return std::nullopt;

        auto end = findMatching(json, second, '[', ']');
        if (end == std::string::npos) return std::nullopt;

        return json.substr(second, end - second + 1);
    }

    static std::optional<std::string> readJsonStringField(
        const std::string& json,
        const std::string& key
    ) {
        auto value = readJsonValueField(json, key);
        if (!value.has_value()) return std::nullopt;

        std::string normalized = trim(*value);
        if (!isJsonString(normalized)) return std::nullopt;
        return unquote(normalized);
    }

    static std::optional<bool> readJsonBoolField(
        const std::string& json,
        const std::string& key
    ) {
        auto value = readJsonValueField(json, key);
        if (!value.has_value()) return std::nullopt;

        std::string normalized = trim(*value);
        if (normalized == "true") return true;
        if (normalized == "false") return false;

        return std::nullopt;
    }

    static std::optional<std::size_t> readJsonIntegerField(
        const std::string& json,
        const std::string& key
    ) {
        auto value = readJsonValueField(json, key);
        if (!value.has_value()) return std::nullopt;

        std::string normalized = trim(*value);
        if (normalized.empty() || normalized.front() == '"') return std::nullopt;

        std::size_t end = 0;
        while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end]))) {
            ++end;
        }

        if (end == 0) return std::nullopt;
        return static_cast<std::size_t>(std::stoull(normalized.substr(0, end)));
    }

    static std::optional<std::string> readJsonValueField(
        const std::string& json,
        const std::string& key
    ) {
        std::string object = trim(json);
        auto objStart = object.find('{');
        if (objStart == std::string::npos) return std::nullopt;

        auto objEnd = findMatching(object, objStart, '{', '}');
        if (objEnd == std::string::npos) return std::nullopt;

        std::size_t pos = objStart + 1;
        while (pos < objEnd) {
            while (pos < objEnd &&
                   (std::isspace(static_cast<unsigned char>(object[pos])) || object[pos] == ',')) {
                ++pos;
            }

            if (pos >= objEnd) break;
            if (object[pos] != '"') {
                ++pos;
                continue;
            }

            auto keyEnd = findStringEnd(object, pos);
            if (keyEnd == std::string::npos || keyEnd >= objEnd) return std::nullopt;

            std::string currentKey = object.substr(pos + 1, keyEnd - pos - 1);
            pos = keyEnd + 1;

            while (pos < objEnd && std::isspace(static_cast<unsigned char>(object[pos]))) {
                ++pos;
            }

            if (pos >= objEnd || object[pos] != ':') return std::nullopt;
            ++pos;

            while (pos < objEnd && std::isspace(static_cast<unsigned char>(object[pos]))) {
                ++pos;
            }

            if (pos >= objEnd) return std::nullopt;

            auto valueEnd = findJsonValueEnd(object, pos, objEnd);
            if (valueEnd == std::string::npos) return std::nullopt;

            if (currentKey == key) {
                return object.substr(pos, valueEnd - pos);
            }

            pos = valueEnd;
        }

        return std::nullopt;
    }

    static std::size_t findStringEnd(const std::string& s, std::size_t open) {
        bool esc = false;
        for (std::size_t i = open + 1; i < s.size(); ++i) {
            if (esc) {
                esc = false;
                continue;
            }

            if (s[i] == '\\') {
                esc = true;
                continue;
            }

            if (s[i] == '"') {
                return i;
            }
        }

        return std::string::npos;
    }

    static std::size_t findJsonValueEnd(
        const std::string& s,
        std::size_t valueStart,
        std::size_t objectEnd
    ) {
        if (valueStart >= objectEnd) return std::string::npos;

        if (s[valueStart] == '"') {
            auto end = findStringEnd(s, valueStart);
            return end == std::string::npos ? std::string::npos : end + 1;
        }

        if (s[valueStart] == '[') {
            auto end = findMatching(s, valueStart, '[', ']');
            return end == std::string::npos ? std::string::npos : end + 1;
        }

        if (s[valueStart] == '{') {
            auto end = findMatching(s, valueStart, '{', '}');
            return end == std::string::npos ? std::string::npos : end + 1;
        }

        std::size_t pos = valueStart;
        while (pos < objectEnd && s[pos] != ',') {
            ++pos;
        }

        while (pos > valueStart && std::isspace(static_cast<unsigned char>(s[pos - 1]))) {
            --pos;
        }

        return pos;
    }

    static std::size_t findMatching(
        const std::string& s,
        std::size_t open,
        char left,
        char right
    ) {
        int depth = 0;
        bool inString = false;
        bool esc = false;

        for (std::size_t i = open; i < s.size(); ++i) {
            char c = s[i];

            if (inString) {
                if (esc) {
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    inString = false;
                }
                continue;
            }

            if (c == '"') {
                inString = true;
                continue;
            }

            if (c == left) ++depth;
            if (c == right) {
                --depth;
                if (depth == 0) return i;
            }
        }

        return std::string::npos;
    }

    static std::string trim(const std::string& s) {
        std::size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;

        std::size_t b = s.size();
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;

        return s.substr(a, b - a);
    }

    static bool startsWith(
        const std::string& s,
        const std::string& prefix
    ) {
        return s.rfind(prefix, 0) == 0;
    }

    static bool isJsonString(const std::string& s) {
        return s.size() >= 2 && s.front() == '"' && s.back() == '"';
    }

    static std::string unquote(const std::string& s) {
        if (!isJsonString(s)) return s;
        return s.substr(1, s.size() - 2);
    }

private:
    TypeResolver resolver_;
    ProtoDefVariableMap variables_;
};

}
