#pragma once

#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/MsalCachePlugin.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {

// Bounded native model of @azure/msal-node 2.16.3 TokenCache's
// ISerializableTokenCache surface. Entities use the serialized (snake_case)
// shapes emitted by msal-node's Serializer. JsRuntimeValue copies retain the
// identity of object/array nodes, just as assigning a JavaScript value does.
class MsalSerializableTokenCache final : public ISerializableTokenCache {
public:
    enum class EntityMap {
        Account,
        AccessToken,
        RefreshToken,
        IdToken,
        AppMetadata
    };

    MsalSerializableTokenCache();
    explicit MsalSerializableTokenCache(
        const std::optional<std::string>& serializedCache
    );

    void deserialize(
        const std::optional<std::string>& serializedCache
    ) override;
    std::string serialize() override;

    // The plugin resolves a method before its first await. Return a callable
    // snapshot which invokes this exact token-cache receiver later.
    DeserializeMethod resolveDeserializeMethod() override;
    SerializeMethod resolveSerializeMethod() override;

    bool hasChanged() const noexcept { return cacheHasChanged_; }
    void markChanged() noexcept { cacheHasChanged_ = true; }
    void markUnchanged() noexcept { cacheHasChanged_ = false; }

    // Live classified view of the flat NodeStorage KV store, in Serializer
    // order:
    // Account, IdToken, AccessToken, RefreshToken, AppMetadata.
    // The returned copy shares the root node and all nested identities.
    // Mutating entity fields is live; structural root/map edits are a C++
    // extension and must use the methods below to update flat-KV metadata.
    JsRuntimeValue state() const noexcept { return state_; }

    // Parsed raw persistence snapshot. This is undefined when deserialize was
    // passed JavaScript undefined or an empty string. It is kept separately
    // because MSAL does not replace its in-memory maps in those two cases.
    JsRuntimeValue snapshot() const noexcept { return snapshot_; }
    const std::optional<std::string>& serializedSnapshot() const noexcept {
        return cacheSnapshot_;
    }

    // Produces the exact state serialize() would stringify, without clearing
    // cacheHasChanged. Unknown snapshot roots and retained unknown entity
    // fields participate in the same removal/update merge as msal-node.
    JsRuntimeValue materialize() const;

    // Returns the current classified map object. Entity values retain their
    // live object identity. Structural mutation of this returned map is a C++
    // extension and is not authoritative for NodeStorage flat-KV collision
    // tracking; use set/remove/overlay methods for structural changes.
    JsRuntimeValue entityMap(EntityMap map) const;
    void replaceEntityMap(EntityMap map, JsRuntimeValue entities);

    bool hasEntity(EntityMap map, std::string_view key) const;
    JsRuntimeValue getEntity(EntityMap map, std::string_view key) const;
    void setEntity(EntityMap map, std::string key, JsRuntimeValue entity);
    bool removeEntity(EntityMap map, std::string_view key);
    void clearEntityMap(EntityMap map);

    // JavaScript object-spread overlays. Values are copied by value while
    // nested object/array identities remain shared. overlayEntity replaces
    // the entry with the newly created object, matching `{ ...old, ...new }`.
    JsRuntimeValue overlayEntity(
        EntityMap map,
        std::string key,
        const JsRuntimeValue& overlay
    );
    void overlayEntityMap(EntityMap map, const JsRuntimeValue& overlay);

    static std::string_view entityMapName(EntityMap map) noexcept;

private:
    struct FlatEntry {
        std::string key;
        EntityMap sourceMap = EntityMap::Account;
        JsRuntimeValue entity = JsRuntimeValue::undefined();
    };

    void upsertFlat(
        EntityMap sourceMap,
        std::string key,
        JsRuntimeValue entity
    );
    JsRuntimeValue classifiedState() const;
    void rebuildClassifiedState();

    JsRuntimeValue state_;
    JsRuntimeValue snapshot_ = JsRuntimeValue::undefined();
    std::optional<std::string> cacheSnapshot_;
    std::vector<FlatEntry> flatEntries_;
    bool cacheHasChanged_ = false;
};

} // namespace bedrock
