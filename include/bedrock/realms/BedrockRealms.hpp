#pragma once

#include <bedrock/auth/Authflow.hpp>
#include <bedrock/auth/JsRuntimeValue.hpp>
#include <bedrock/auth/XboxTokenManager.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bedrock {

class BedrockRealmApi;

struct BedrockRealmAddress {
    std::string host;
    uint16_t port = 0;
};

struct BedrockRealmInvite {
    std::string inviteCode;
    std::string ownerXUID;
    std::string type;
    double createdOn = 0.0;
    std::string inviteLink;
    std::string deepLinkUrl;
};

struct BedrockPendingRealmInvite {
    std::string invitationId;
    std::string worldName;
    std::string worldDescription;
    JsRuntimeValue worldOwnerName = JsRuntimeValue::undefined();
    std::string worldOwnerXUID;
    double createdOn = 0.0;
};

struct BedrockRealmDownload {
    std::string downloadUrl;
    std::string token;
    double size = 0.0;
    std::string fileExtension = ".mcworld";

    std::vector<uint8_t> getBuffer() const;
    void writeToDirectory(const std::filesystem::path& directory) const;

private:
    friend class BedrockRealmApi;
    std::shared_ptr<BedrockRealmApi> api_;
};

struct BedrockRealmBackupMetadata {
    std::string gameDifficulty;
    std::string name;
    std::string gameServerVersion;
    JsRuntimeValue enabledPacks = JsRuntimeValue::undefined();
    JsRuntimeValue description = JsRuntimeValue::undefined();
    std::string gamemode;
    std::string worldType;
};

struct BedrockRealmBackup {
    std::string id;
    double lastModifiedDate = 0.0;
    double size = 0.0;
    BedrockRealmBackupMetadata metadata;

    BedrockRealmDownload getDownload() const;
    JsRuntimeValue restore() const;

private:
    friend class BedrockRealmApi;
    std::shared_ptr<BedrockRealmApi> api_;
    int64_t realmId_ = 0;
    int64_t slotId_ = 0;
};

struct BedrockRealm {
    int64_t id = 0;
    std::string remoteSubscriptionId;
    std::string owner;
    std::string ownerUUID;
    std::string name;
    std::string motd;
    std::string defaultPermission;
    std::string state;
    double daysLeft = 0.0;
    bool expired = false;
    bool expiredTrial = false;
    bool gracePeriod = false;
    std::string worldType;
    JsRuntimeValue players = JsRuntimeValue::undefined();
    int64_t maxPlayers = 0;
    JsRuntimeValue minigameName = JsRuntimeValue::undefined();
    JsRuntimeValue minigameId = JsRuntimeValue::undefined();
    JsRuntimeValue minigameImage = JsRuntimeValue::undefined();
    int64_t activeSlot = 0;
    JsRuntimeValue slots = JsRuntimeValue::undefined();
    bool member = false;
    int64_t clubId = 0;
    JsRuntimeValue subscriptionRefreshStatus = JsRuntimeValue::undefined();
    JsRuntimeValue raw = JsRuntimeValue::undefined();

    BedrockRealmAddress getAddress() const;
    BedrockRealm invitePlayer(const std::string& uuid) const;
    JsRuntimeValue open() const;
    JsRuntimeValue close() const;
    void deleteRealm() const;
    BedrockRealmDownload getWorldDownload() const;
    std::vector<BedrockRealmBackup> getBackups() const;
    JsRuntimeValue getSubscriptionInfo(bool detailed = false) const;
    JsRuntimeValue changeActiveSlot(int64_t slotId) const;
    void changeNameAndDescription(
        const std::string& name,
        const std::string& description
    ) const;

private:
    friend class BedrockRealmApi;
    std::shared_ptr<BedrockRealmApi> api_;
};

struct BedrockRealmsOptions {
    bool usePreview = false;
    bool skipAuth = false;
    int maxRetries = 4;
    XboxTokenHttpClientPtr httpClient;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue;
    std::function<void(std::chrono::milliseconds)> delay;
};

// Bedrock-only port of prismarine-realms 1.4.1. Every public method mirrors
// the corresponding Bedrock/common endpoint; no Java Realm API is included.
class BedrockRealmApi final :
    public std::enable_shared_from_this<BedrockRealmApi> {
public:
    inline static constexpr const char* Host =
        "https://pocket.realms.minecraft.net";
    inline static constexpr const char* UserAgent = "MCPE/UWP";
    inline static constexpr const char* RelyingParty =
        "https://pocket.realms.minecraft.net/";

    static std::shared_ptr<BedrockRealmApi> from(
        AuthflowPtr authflow,
        BedrockRealmsOptions options = {}
    );

    BedrockRealm getRealm(int64_t realmId);
    std::vector<BedrockRealm> getRealms();
    std::vector<BedrockRealmBackup> getRealmBackups(
        int64_t realmId,
        int64_t slotId
    );
    JsRuntimeValue restoreRealmFromBackup(
        int64_t realmId,
        const std::string& backupId
    );
    JsRuntimeValue getRealmSubscriptionInfo(
        int64_t realmId,
        bool detailed = false
    );
    JsRuntimeValue changeRealmState(
        int64_t realmId,
        const std::string& state
    );
    JsRuntimeValue changeRealmActiveSlot(int64_t realmId, int64_t slotId);
    void changeRealmNameAndDescription(
        int64_t realmId,
        const std::string& name,
        const std::string& description
    );
    void deleteRealm(int64_t realmId);

    BedrockRealmAddress getRealmAddress(int64_t realmId);
    BedrockRealm getRealmFromInvite(
        const std::string& realmInviteCode,
        bool invite = true
    );
    BedrockRealmInvite getRealmInvite(int64_t realmId);
    BedrockRealmInvite refreshRealmInvite(int64_t realmId);
    JsRuntimeValue getPendingInviteCount();
    std::vector<BedrockPendingRealmInvite> getPendingInvites();
    void acceptRealmInvitation(const std::string& invitationId);
    void rejectRealmInvitation(const std::string& invitationId);
    BedrockRealm acceptRealmInviteFromCode(const std::string& inviteCode);
    BedrockRealm invitePlayer(
        int64_t realmId,
        const std::string& uuid
    );
    BedrockRealmDownload getRealmWorldDownload(
        int64_t realmId,
        int64_t slotId,
        const std::string& backupId = "latest"
    );
    void resetRealm(int64_t realmId);
    BedrockRealm removePlayerFromRealm(
        int64_t realmId,
        const std::string& xuid
    );
    BedrockRealm opRealmPlayer(
        int64_t realmId,
        const std::string& uuid
    );
    BedrockRealm deopRealmPlayer(
        int64_t realmId,
        const std::string& uuid
    );
    void banPlayerFromRealm(int64_t realmId, const std::string& uuid);
    void unbanPlayerFromRealm(int64_t realmId, const std::string& uuid);
    void removeRealmFromJoinedList(int64_t realmId);
    void changeIsTexturePackRequired(int64_t realmId, bool forced);
    void changeRealmDefaultPermission(
        int64_t realmId,
        std::string permission
    );
    void changeRealmPlayerPermission(
        int64_t realmId,
        std::string permission,
        const std::string& uuid
    );

    std::vector<uint8_t> downloadWorld(const BedrockRealmDownload& value);

private:
    BedrockRealmApi(AuthflowPtr authflow, BedrockRealmsOptions options);

    JsRuntimeValue request(
        std::string method,
        std::string route,
        JsRuntimeValue body = JsRuntimeValue::undefined()
    );
    JsRuntimeValue execute(
        const XboxTokenHttpRequest& request,
        int retries = 0
    );
    BedrockRealm makeRealm(const JsRuntimeValue& value);
    BedrockRealmBackup makeBackup(
        int64_t realmId,
        int64_t slotId,
        const JsRuntimeValue& value
    );
    BedrockRealmDownload makeDownload(const JsRuntimeValue& value);

    AuthflowPtr authflow_;
    BedrockRealmsOptions options_;
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue_;
    XboxTokenHttpClientPtr httpClient_;
};

class BedrockRealmId {
public:
    BedrockRealmId() = default;

    BedrockRealmId& operator=(int64_t value) noexcept {
        value_ = value;
        return *this;
    }

    BedrockRealmId& operator=(const char* value) {
        return *this = std::string(value ? value : "");
    }

    BedrockRealmId& operator=(std::string value) {
        if (value.empty()) {
            value_.reset();
            return *this;
        }
        std::size_t consumed = 0;
        const auto parsed = std::stoll(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("Invalid Bedrock Realm id: " + value);
        }
        value_ = parsed;
        return *this;
    }

    bool has_value() const noexcept { return value_.has_value(); }
    int64_t value() const { return value_.value(); }
    int64_t operator*() const { return value(); }
    void reset() noexcept { value_.reset(); }

private:
    std::optional<int64_t> value_;
};

// createClient's JavaScript `realms` object. Assigning true remains source
// compatible with the old bool field; specifying any selector also enables
// the Realm preflight. addressResolver is a C++ integration/test seam and is
// skipped by the production path when empty.
struct BedrockRealmSelection {
    bool enabled = false;
    BedrockRealmId realmId;
    std::string realmInvite;
    std::function<std::optional<BedrockRealm>(
        const std::vector<BedrockRealm>&
    )> pickRealm;
    std::function<std::future<BedrockRealm>(
        const std::vector<BedrockRealm>&
    )> pickRealmAsync;
    bool usePreview = false;
    int maxRetries = 4;
    std::function<BedrockRealmAddress(AuthflowPtr)> addressResolver;

    BedrockRealmSelection& operator=(bool value) {
        enabled = value;
        if (!value) {
            realmId.reset();
            realmInvite.clear();
            pickRealm = {};
            pickRealmAsync = {};
            addressResolver = {};
            usePreview = false;
            maxRetries = 4;
        }
        return *this;
    }

    explicit operator bool() const noexcept {
        return enabled || realmId.has_value() || !realmInvite.empty() ||
            static_cast<bool>(pickRealm) || static_cast<bool>(pickRealmAsync) ||
            static_cast<bool>(addressResolver);
    }
};

BedrockRealmAddress resolveBedrockRealmAddress(
    AuthflowPtr authflow,
    const BedrockRealmSelection& selection
);

} // namespace bedrock
