#include <bedrock/auth/MsalSerializableTokenCache.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bedrock {
namespace {

using EntityMap = MsalSerializableTokenCache::EntityMap;

constexpr std::array<EntityMap, 5> serializerMapOrder {
    EntityMap::Account,
    EntityMap::IdToken,
    EntityMap::AccessToken,
    EntityMap::RefreshToken,
    EntityMap::AppMetadata
};

constexpr std::array<EntityMap, 5> removalMergeMapOrder {
    EntityMap::Account,
    EntityMap::AccessToken,
    EntityMap::RefreshToken,
    EntityMap::IdToken,
    EntityMap::AppMetadata
};

constexpr std::array<std::string_view, 11> accountFields {
    "home_account_id",
    "environment",
    "realm",
    "local_account_id",
    "username",
    "authority_type",
    "name",
    "client_info",
    "last_modification_time",
    "last_modification_app",
    "tenantProfiles"
};

constexpr std::array<std::string_view, 6> idTokenFields {
    "home_account_id",
    "environment",
    "credential_type",
    "client_id",
    "secret",
    "realm"
};

constexpr std::array<std::string_view, 16> accessTokenFields {
    "home_account_id",
    "environment",
    "credential_type",
    "client_id",
    "secret",
    "realm",
    "target",
    "cached_at",
    "expires_on",
    "extended_expires_on",
    "refresh_on",
    "key_id",
    "token_type",
    "requestedClaims",
    "requestedClaimsHash",
    "userAssertionHash"
};

constexpr std::array<std::string_view, 8> refreshTokenFields {
    "home_account_id",
    "environment",
    "credential_type",
    "client_id",
    "secret",
    "family_id",
    "target",
    "realm"
};

constexpr std::array<std::string_view, 3> appMetadataFields {
    "client_id",
    "environment",
    "family_id"
};

bool canonicalArrayIndex(std::string_view key, std::uint32_t& result) {
    if (key.empty() || (key.size() > 1 && key.front() == '0')) return false;
    std::uint64_t parsed = 0;
    const auto conversion = std::from_chars(
        key.data(),
        key.data() + key.size(),
        parsed
    );
    if (conversion.ec != std::errc() ||
        conversion.ptr != key.data() + key.size() ||
        parsed > 4294967294ULL) {
        return false;
    }
    result = static_cast<std::uint32_t>(parsed);
    return true;
}

std::vector<std::size_t> ecmaKeyOrder(
    const std::vector<std::string>& keys
) {
    struct IndexedKey {
        std::uint32_t value = 0;
        std::size_t position = 0;
    };
    std::vector<IndexedKey> indices;
    std::vector<std::size_t> strings;
    for (std::size_t position = 0; position < keys.size(); ++position) {
        std::uint32_t index = 0;
        if (canonicalArrayIndex(keys[position], index)) {
            indices.push_back(IndexedKey {index, position});
        } else {
            strings.push_back(position);
        }
    }
    std::sort(indices.begin(), indices.end(), [](const auto& left,
                                                  const auto& right) {
        return left.value < right.value;
    });

    std::vector<std::size_t> result;
    result.reserve(keys.size());
    for (const auto& index : indices) result.push_back(index.position);
    result.insert(result.end(), strings.begin(), strings.end());
    return result;
}

std::vector<std::uint16_t> utf16CodeUnits(std::string_view input) {
    std::vector<std::uint16_t> result;
    for (std::size_t offset = 0; offset < input.size();) {
        const auto first = static_cast<std::uint8_t>(input[offset]);
        std::uint32_t codePoint = first;
        std::size_t width = 1;
        const auto continuation = [&](std::size_t index) {
            return index < input.size() &&
                (static_cast<std::uint8_t>(input[index]) & 0xc0U) == 0x80U;
        };

        if (first >= 0xc2U && first <= 0xdfU && continuation(offset + 1)) {
            codePoint = ((first & 0x1fU) << 6) |
                (static_cast<std::uint8_t>(input[offset + 1]) & 0x3fU);
            width = 2;
        } else if (first >= 0xe0U && first <= 0xefU &&
            continuation(offset + 1) && continuation(offset + 2)) {
            codePoint = ((first & 0x0fU) << 12) |
                ((static_cast<std::uint8_t>(input[offset + 1]) & 0x3fU)
                    << 6) |
                (static_cast<std::uint8_t>(input[offset + 2]) & 0x3fU);
            width = 3;
        } else if (first >= 0xf0U && first <= 0xf4U &&
            continuation(offset + 1) && continuation(offset + 2) &&
            continuation(offset + 3)) {
            codePoint = ((first & 0x07U) << 18) |
                ((static_cast<std::uint8_t>(input[offset + 1]) & 0x3fU)
                    << 12) |
                ((static_cast<std::uint8_t>(input[offset + 2]) & 0x3fU)
                    << 6) |
                (static_cast<std::uint8_t>(input[offset + 3]) & 0x3fU);
            width = 4;
        }

        if (codePoint <= 0xffffU) {
            result.push_back(static_cast<std::uint16_t>(codePoint));
        } else {
            codePoint -= 0x10000U;
            result.push_back(static_cast<std::uint16_t>(
                0xd800U + (codePoint >> 10)
            ));
            result.push_back(static_cast<std::uint16_t>(
                0xdc00U + (codePoint & 0x3ffU)
            ));
        }
        offset += width;
    }
    return result;
}

std::string wtf8CodeUnit(std::uint16_t unit) {
    std::string result;
    if (unit <= 0x7fU) {
        result.push_back(static_cast<char>(unit));
    } else if (unit <= 0x7ffU) {
        result.push_back(static_cast<char>(0xc0U | (unit >> 6)));
        result.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    } else {
        result.push_back(static_cast<char>(0xe0U | (unit >> 12)));
        result.push_back(static_cast<char>(0x80U | ((unit >> 6) & 0x3fU)));
        result.push_back(static_cast<char>(0x80U | (unit & 0x3fU)));
    }
    return result;
}

std::vector<JsRuntimeProperty> enumerableProperties(
    const JsRuntimeValue& value
) {
    if (value.isObject() || value.isArray()) return value.ownProperties();
    std::vector<JsRuntimeProperty> result;
    if (!value.isString()) return result;

    const auto units = utf16CodeUnits(value.stringValue());
    result.reserve(units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        result.emplace_back(
            std::to_string(index),
            JsRuntimeValue::string(wtf8CodeUnit(units[index]))
        );
    }
    return result;
}

std::size_t parseErrorOffset(std::string_view message) {
    constexpr std::string_view marker = "JSON parse error at offset ";
    const auto markerPosition = message.find(marker);
    if (markerPosition == std::string_view::npos) return 0;
    const auto begin = markerPosition + marker.size();
    std::size_t result = 0;
    const auto conversion = std::from_chars(
        message.data() + begin,
        message.data() + message.size(),
        result
    );
    return conversion.ec == std::errc() ? result : 0;
}

[[noreturn]] void throwNodeJsonParseError(
    std::string_view input,
    std::string_view nativeMessage
) {
    std::size_t first = 0;
    while (first < input.size() &&
        (input[first] == ' ' || input[first] == '\t' ||
         input[first] == '\n' || input[first] == '\r')) {
        ++first;
    }
    if (first == input.size()) {
        throw std::runtime_error("Unexpected end of JSON input");
    }

    std::size_t offset = parseErrorOffset(nativeMessage);
    const auto literalMismatch = [&](std::string_view literal) {
        std::size_t position = 0;
        while (position < literal.size() &&
            first + position < input.size() &&
            input[first + position] == literal[position]) {
            ++position;
        }
        return position;
    };
    std::string_view literal;
    if (input[first] == 'n') literal = "null";
    if (input[first] == 't') literal = "true";
    if (input[first] == 'f') literal = "false";
    if (!literal.empty() && offset == first) {
        const auto mismatch = literalMismatch(literal);
        if (first + mismatch >= input.size()) {
            throw std::runtime_error("Unexpected end of JSON input");
        }
        offset = first + mismatch;
    }

    if (offset >= input.size()) {
        throw std::runtime_error("Unexpected end of JSON input");
    }

    const bool leadingZero =
        ((input[first] == '0' && offset == first + 1) ||
         (input[first] == '-' && first + 1 < input.size() &&
          input[first + 1] == '0' && offset == first + 2)) &&
        input[offset] >= '0' && input[offset] <= '9';
    if (leadingZero) {
        throw std::runtime_error(
            "Unexpected number in JSON at position " +
            std::to_string(offset)
        );
    }

    throw std::runtime_error(
        std::string("Unexpected token ") + input[offset] +
        " in JSON at position " + std::to_string(offset)
    );
}

JsRuntimeValue parseJsonLikeNode(std::string_view input) {
    try {
        return JsRuntimeJson::parse(input);
    } catch (const std::exception& error) {
        throwNodeJsonParseError(input, error.what());
    }
}

template <std::size_t Size>
JsRuntimeValue projectEntity(
    const JsRuntimeValue& entity,
    const std::array<std::string_view, Size>& fields
) {
    if (entity.isNull() || entity.isUndefined()) {
        throw std::runtime_error(
            std::string("Cannot read properties of ") +
            (entity.isNull() ? "null" : "undefined") + " (reading '" +
            std::string(fields.front()) + "')"
        );
    }

    auto projected = JsRuntimeValue::object();
    for (const auto field : fields) {
        const auto* value = entity.get(field);
        projected.set(
            std::string(field),
            value ? *value : JsRuntimeValue::undefined()
        );
    }
    return projected;
}

JsRuntimeValue projectEntity(EntityMap map, const JsRuntimeValue& entity) {
    switch (map) {
        case EntityMap::Account:
            return projectEntity(entity, accountFields);
        case EntityMap::IdToken:
            return projectEntity(entity, idTokenFields);
        case EntityMap::AccessToken:
            return projectEntity(entity, accessTokenFields);
        case EntityMap::RefreshToken:
            return projectEntity(entity, refreshTokenFields);
        case EntityMap::AppMetadata:
            return projectEntity(entity, appMetadataFields);
    }
    throw std::logic_error("unknown MSAL token-cache entity map");
}

std::string jsStringForJsonParse(const JsRuntimeValue& value);

std::string jsArrayToString(const JsRuntimeValue& value) {
    std::string result;
    const auto& elements = value.arrayNode()->elements();
    for (std::size_t index = 0; index < elements.size(); ++index) {
        if (index) result.push_back(',');
        if (!elements[index] || elements[index]->isUndefined() ||
            elements[index]->isNull()) {
            continue;
        }
        result += jsStringForJsonParse(*elements[index]);
    }
    return result;
}

std::string jsStringForJsonParse(const JsRuntimeValue& value) {
    if (value.isString()) return value.stringValue();
    if (value.isNull()) return "null";
    if (value.isUndefined()) return "undefined";
    if (value.isBool()) return value.boolValue() ? "true" : "false";
    if (value.isNumber()) {
        return JsRuntimeJson::stringify(value).value_or("undefined");
    }
    if (value.isArray()) return jsArrayToString(value);
    return "[object Object]";
}

JsRuntimeValue deserializeEntity(EntityMap map, const JsRuntimeValue& entity) {
    auto projected = projectEntity(map, entity);
    if (map != EntityMap::Account) return projected;

    // Deserializer parses each serialized tenant profile and Serializer
    // stringifies it again. Besides rebuilding the array, this normalizes the
    // JSON text exactly as the two installed MSAL functions do.
    const auto* tenantProfiles = entity.get("tenantProfiles");
    if (!tenantProfiles || tenantProfiles->isUndefined() ||
        tenantProfiles->isNull()) {
        projected.set("tenantProfiles", JsRuntimeValue::undefined());
        return projected;
    }
    if (!tenantProfiles->isArray()) {
        throw std::runtime_error(
            "serializedAcc.tenantProfiles?.map is not a function"
        );
    }

    auto normalized = JsRuntimeValue::array();
    for (const auto& entry : tenantProfiles->arrayNode()->elements()) {
        const auto argument = entry
            ? jsStringForJsonParse(*entry)
            : std::string("undefined");
        const auto parsed = parseJsonLikeNode(argument);
        const auto serialized = JsRuntimeJson::stringify(parsed);
        if (!serialized) {
            throw std::runtime_error(
                "serialized tenant profile is not JSON-serializable"
            );
        }
        normalized.push(JsRuntimeValue::string(*serialized));
    }
    projected.set("tenantProfiles", std::move(normalized));
    return projected;
}

void spreadEnumerableObject(
    JsRuntimeValue& destination,
    const JsRuntimeValue& source
) {
    if (!destination.isObject()) {
        throw std::logic_error("object-spread destination is not an object");
    }
    for (const auto& property : enumerableProperties(source)) {
        destination.set(property.key, property.value);
    }
}

JsRuntimeValue freshState() {
    auto result = JsRuntimeValue::object();
    for (const auto map : serializerMapOrder) {
        result.set(
            std::string(MsalSerializableTokenCache::entityMapName(map)),
            JsRuntimeValue::object()
        );
    }
    return result;
}

const JsRuntimeValue* mapFromRoot(
    const JsRuntimeValue& root,
    EntityMap map
) {
    return root.get(MsalSerializableTokenCache::entityMapName(map));
}

JsRuntimeValue deserializeMap(
    EntityMap map,
    const JsRuntimeValue* serializedMap
) {
    auto result = JsRuntimeValue::object();
    if (!serializedMap || !serializedMap->truthy()) return result;
    for (const auto& property : enumerableProperties(*serializedMap)) {
        result.set(property.key, deserializeEntity(map, property.value));
    }
    return result;
}

JsRuntimeValue serializeMap(EntityMap map, const JsRuntimeValue& liveMap) {
    if (!liveMap.isObject() && !liveMap.isArray()) {
        throw std::runtime_error("MSAL token-cache entity map is not an object");
    }
    auto result = JsRuntimeValue::object();
    for (const auto& property : liveMap.ownProperties()) {
        result.set(property.key, projectEntity(map, property.value));
    }
    return result;
}

JsRuntimeValue serializeCurrentState(const JsRuntimeValue& liveState) {
    auto result = JsRuntimeValue::object();
    for (const auto map : serializerMapOrder) {
        const auto* liveMap = mapFromRoot(liveState, map);
        result.set(
            std::string(MsalSerializableTokenCache::entityMapName(map)),
            liveMap ? serializeMap(map, *liveMap) : JsRuntimeValue::object()
        );
    }
    return result;
}

bool stringPropertyEquals(
    const JsRuntimeValue& entity,
    std::string_view property,
    std::string_view expected
) {
    const auto* value = entity.get(property);
    return value && value->isString() && value->stringValue() == expected;
}

bool hasCredentialProperties(const JsRuntimeValue& entity) {
    return entity.hasOwn("home_account_id") &&
        entity.hasOwn("environment") &&
        entity.hasOwn("credential_type") &&
        entity.hasOwn("client_id") &&
        entity.hasOwn("secret");
}

std::optional<EntityMap> classifyFlatEntity(
    EntityMap sourceMap,
    std::string_view key,
    const JsRuntimeValue& entity
) {
    // Deserializer creates an AccountEntity instance for every Account map
    // value, even a boxed primitive. cacheToInMemoryCache checks instanceof
    // AccountEntity before all structural token classifiers.
    if (sourceMap == EntityMap::Account) return EntityMap::Account;
    if (!entity.isObject() && !entity.isArray()) return std::nullopt;

    const bool credential = hasCredentialProperties(entity);
    if (credential && entity.hasOwn("realm") && stringPropertyEquals(
            entity,
            "credential_type",
            "IdToken"
        )) {
        return EntityMap::IdToken;
    }
    if (credential && entity.hasOwn("realm") && entity.hasOwn("target") &&
        (stringPropertyEquals(entity, "credential_type", "AccessToken") ||
         stringPropertyEquals(
             entity,
             "credential_type",
             "AccessToken_With_AuthScheme"
         ))) {
        return EntityMap::AccessToken;
    }
    if (credential && stringPropertyEquals(
            entity,
            "credential_type",
            "RefreshToken"
        )) {
        return EntityMap::RefreshToken;
    }
    if (key.starts_with("appmetadata") && entity.hasOwn("client_id") &&
        entity.hasOwn("environment")) {
        return EntityMap::AppMetadata;
    }
    return std::nullopt;
}

JsRuntimeValue removalFilteredMap(
    const JsRuntimeValue* oldMap,
    const JsRuntimeValue* currentMap
) {
    if (!oldMap) return JsRuntimeValue::undefined();
    if (!oldMap->truthy()) return *oldMap;

    auto filtered = JsRuntimeValue::object();
    spreadEnumerableObject(filtered, *oldMap);
    if (!currentMap || (!currentMap->isObject() && !currentMap->isArray())) {
        for (const auto& property : filtered.ownProperties()) {
            filtered.objectNode()->erase(property.key);
        }
        return filtered;
    }

    for (const auto& property : filtered.ownProperties()) {
        if (currentMap->hasOwn("hasOwnProperty")) {
            throw std::runtime_error(
                "newState.hasOwnProperty is not a function"
            );
        }
        if (!currentMap->hasOwn(property.key)) {
            filtered.objectNode()->erase(property.key);
        }
    }
    return filtered;
}

void mergeUpdates(JsRuntimeValue& oldState, const JsRuntimeValue& newState) {
    if (!newState.isObject() && !newState.isArray()) return;

    if (!oldState.isObject() && !oldState.isArray()) {
        for (const auto& property : newState.ownProperties()) {
            if (property.value.isNull()) continue;

            std::string kind = "primitive";
            std::string rendered;
            if (oldState.isString()) {
                kind = "string";
                rendered = oldState.stringValue();
            } else if (oldState.isNumber()) {
                kind = "number";
                rendered = JsRuntimeJson::stringify(oldState).value_or("NaN");
            } else if (oldState.isBool()) {
                kind = "boolean";
                rendered = oldState.boolValue() ? "true" : "false";
            }
            throw std::runtime_error(
                "Cannot create property '" + property.key + "' on " + kind +
                " '" + rendered + "'"
            );
        }
        return;
    }

    for (const auto& property : newState.ownProperties()) {
        if (oldState.hasOwn("hasOwnProperty")) {
            throw std::runtime_error(
                "oldState.hasOwnProperty is not a function"
            );
        }
        auto* oldValue = oldState.get(property.key);
        if (!oldValue) {
            if (!property.value.isNull()) {
                oldState.set(property.key, property.value);
            }
            continue;
        }

        const bool recurse = property.value.isObject() &&
            !oldValue->isUndefined() && !oldValue->isNull();
        if (recurse) {
            mergeUpdates(*oldValue, property.value);
        } else {
            oldState.set(property.key, property.value);
        }
    }
}

JsRuntimeValue mergeSnapshot(
    const JsRuntimeValue& oldSnapshot,
    const JsRuntimeValue& currentState
) {
    // TokenCache.mergeRemovals returns an object literal. Object spread first
    // preserves every unknown root key, then the five recognized maps are
    // assigned in this exact order.
    auto stateAfterRemoval = JsRuntimeValue::object();
    spreadEnumerableObject(stateAfterRemoval, oldSnapshot);

    for (const auto map : removalMergeMapOrder) {
        stateAfterRemoval.set(
            std::string(MsalSerializableTokenCache::entityMapName(map)),
            removalFilteredMap(
                mapFromRoot(oldSnapshot, map),
                mapFromRoot(currentState, map)
            )
        );
    }

    mergeUpdates(stateAfterRemoval, currentState);
    return stateAfterRemoval;
}

JsRuntimeValue requireLiveMap(const JsRuntimeValue& state, EntityMap map) {
    const auto* value = state.get(
        MsalSerializableTokenCache::entityMapName(map)
    );
    if (!value || !value->isObject()) {
        throw std::runtime_error("MSAL token-cache entity map is not an object");
    }
    return *value;
}

} // namespace

MsalSerializableTokenCache::MsalSerializableTokenCache()
    : state_(freshState()) {}

MsalSerializableTokenCache::MsalSerializableTokenCache(
    const std::optional<std::string>& serializedCache
) : state_(freshState()) {
    deserialize(serializedCache);
}

void MsalSerializableTokenCache::deserialize(
    const std::optional<std::string>& serializedCache
) {
    // TokenCache assigns cacheSnapshot before checking its truthiness.
    cacheSnapshot_ = serializedCache;
    snapshot_ = JsRuntimeValue::undefined();
    if (!cacheSnapshot_ || cacheSnapshot_->empty()) return;

    auto parsed = parseJsonLikeNode(*cacheSnapshot_);
    snapshot_ = parsed;
    if (parsed.isNull()) {
        throw std::runtime_error(
            "Cannot read properties of null (reading 'Account')"
        );
    }

    auto incoming = JsRuntimeValue::object();
    for (const auto map : serializerMapOrder) {
        incoming.set(
            std::string(entityMapName(map)),
            deserializeMap(map, mapFromRoot(parsed, map))
        );
    }

    // NodeStorage.setInMemoryCache converts the five incoming maps to one flat
    // cache using this order. A later map wins a duplicate key without moving
    // that key's original insertion position.
    for (const auto map : serializerMapOrder) {
        const auto* incomingMap = mapFromRoot(incoming, map);
        if (!incomingMap) continue;
        for (const auto& property : incomingMap->ownProperties()) {
            upsertFlat(map, property.key, property.value);
        }
    }
    rebuildClassifiedState();
    // NodeStorage.setInMemoryCache emits a change event, so TokenCache reports
    // hasChanged() after every successful truthy deserialize.
    cacheHasChanged_ = true;
}

std::string MsalSerializableTokenCache::serialize() {
    auto value = materialize();
    const auto serialized = JsRuntimeJson::stringify(value);
    if (!serialized) {
        throw std::runtime_error("MSAL token-cache state is not serializable");
    }
    cacheHasChanged_ = false;
    return *serialized;
}

ISerializableTokenCache::DeserializeMethod
MsalSerializableTokenCache::resolveDeserializeMethod() {
    return [this](const std::optional<std::string>& serializedCache) {
        deserialize(serializedCache);
    };
}

ISerializableTokenCache::SerializeMethod
MsalSerializableTokenCache::resolveSerializeMethod() {
    return [this] { return serialize(); };
}

JsRuntimeValue MsalSerializableTokenCache::materialize() const {
    auto current = serializeCurrentState(classifiedState());
    if (!cacheSnapshot_ || cacheSnapshot_->empty()) return current;

    // msal-node reparses cacheSnapshot on every serialize instead of updating
    // the stored snapshot after a successful merge.
    const auto oldSnapshot = parseJsonLikeNode(*cacheSnapshot_);
    if (oldSnapshot.isNull()) {
        throw std::runtime_error(
            "Cannot read properties of null (reading 'Account')"
        );
    }
    return mergeSnapshot(oldSnapshot, current);
}

JsRuntimeValue MsalSerializableTokenCache::entityMap(EntityMap map) const {
    return requireLiveMap(state_, map);
}

void MsalSerializableTokenCache::replaceEntityMap(
    EntityMap map,
    JsRuntimeValue entities
) {
    if (!entities.isObject()) {
        throw std::invalid_argument("MSAL token-cache entity map must be an object");
    }

    flatEntries_.erase(
        std::remove_if(
            flatEntries_.begin(),
            flatEntries_.end(),
            [map](const auto& entry) {
                return classifyFlatEntity(
                    entry.sourceMap,
                    entry.key,
                    entry.entity
                ) == map;
            }
        ),
        flatEntries_.end()
    );
    for (const auto& property : entities.ownProperties()) {
        upsertFlat(map, property.key, property.value);
    }
    rebuildClassifiedState();
    cacheHasChanged_ = true;
}

bool MsalSerializableTokenCache::hasEntity(
    EntityMap map,
    std::string_view key
) const {
    return requireLiveMap(state_, map).hasOwn(key);
}

JsRuntimeValue MsalSerializableTokenCache::getEntity(
    EntityMap map,
    std::string_view key
) const {
    const auto liveMap = requireLiveMap(state_, map);
    const auto* value = liveMap.get(key);
    return value ? *value : JsRuntimeValue::undefined();
}

void MsalSerializableTokenCache::setEntity(
    EntityMap map,
    std::string key,
    JsRuntimeValue entity
) {
    upsertFlat(map, std::move(key), std::move(entity));
    rebuildClassifiedState();
    cacheHasChanged_ = true;
}

bool MsalSerializableTokenCache::removeEntity(
    EntityMap map,
    std::string_view key
) {
    const auto found = std::find_if(
        flatEntries_.begin(),
        flatEntries_.end(),
        [map, key](const auto& entry) {
            return entry.key == key && classifyFlatEntity(
                entry.sourceMap,
                entry.key,
                entry.entity
            ) == map;
        }
    );
    const bool removed = found != flatEntries_.end();
    if (removed) {
        flatEntries_.erase(found);
        rebuildClassifiedState();
    }
    if (removed) cacheHasChanged_ = true;
    return removed;
}

void MsalSerializableTokenCache::clearEntityMap(EntityMap map) {
    const auto before = flatEntries_.size();
    flatEntries_.erase(
        std::remove_if(
            flatEntries_.begin(),
            flatEntries_.end(),
            [map](const auto& entry) {
                return classifyFlatEntity(
                    entry.sourceMap,
                    entry.key,
                    entry.entity
                ) == map;
            }
        ),
        flatEntries_.end()
    );
    const bool removed = flatEntries_.size() != before;
    if (removed) rebuildClassifiedState();
    if (removed) cacheHasChanged_ = true;
}

JsRuntimeValue MsalSerializableTokenCache::overlayEntity(
    EntityMap map,
    std::string key,
    const JsRuntimeValue& overlay
) {
    auto merged = JsRuntimeValue::object();
    const auto existing = getEntity(map, key);
    spreadEnumerableObject(merged, existing);
    spreadEnumerableObject(merged, overlay);
    setEntity(map, std::move(key), merged);
    return merged;
}

void MsalSerializableTokenCache::overlayEntityMap(
    EntityMap map,
    const JsRuntimeValue& overlay
) {
    if (!overlay.isObject() && !overlay.isArray()) return;
    for (const auto& property : overlay.ownProperties()) {
        upsertFlat(map, property.key, property.value);
    }
    rebuildClassifiedState();
    cacheHasChanged_ = true;
}

void MsalSerializableTokenCache::upsertFlat(
    EntityMap sourceMap,
    std::string key,
    JsRuntimeValue entity
) {
    const auto found = std::find_if(
        flatEntries_.begin(),
        flatEntries_.end(),
        [&key](const auto& entry) { return entry.key == key; }
    );
    if (found != flatEntries_.end()) {
        found->sourceMap = sourceMap;
        found->entity = std::move(entity);
        return;
    }
    flatEntries_.push_back(FlatEntry {
        .key = std::move(key),
        .sourceMap = sourceMap,
        .entity = std::move(entity)
    });
}

JsRuntimeValue MsalSerializableTokenCache::classifiedState() const {
    auto result = freshState();
    std::vector<std::string> keys;
    keys.reserve(flatEntries_.size());
    for (const auto& entry : flatEntries_) keys.push_back(entry.key);

    for (const auto position : ecmaKeyOrder(keys)) {
        const auto& entry = flatEntries_[position];
        const auto classified = classifyFlatEntity(
            entry.sourceMap,
            entry.key,
            entry.entity
        );
        if (!classified) continue;
        auto* map = result.get(entityMapName(*classified));
        if (!map) {
            throw std::logic_error("classified MSAL map is missing");
        }
        map->set(entry.key, entry.entity);
    }
    return result;
}

void MsalSerializableTokenCache::rebuildClassifiedState() {
    state_ = classifiedState();
}

std::string_view MsalSerializableTokenCache::entityMapName(
    EntityMap map
) noexcept {
    switch (map) {
        case EntityMap::Account: return "Account";
        case EntityMap::AccessToken: return "AccessToken";
        case EntityMap::RefreshToken: return "RefreshToken";
        case EntityMap::IdToken: return "IdToken";
        case EntityMap::AppMetadata: return "AppMetadata";
    }
    return "";
}

} // namespace bedrock
