#include <bedrock/realms/BedrockRealms.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace bedrock {

namespace {

const JsRuntimeValue& property(
    const JsRuntimeValue& value,
    std::string_view name
) {
    static const JsRuntimeValue undefined = JsRuntimeValue::undefined();
    const auto* result = value.get(name);
    return result ? *result : undefined;
}

std::string stringProperty(
    const JsRuntimeValue& value,
    std::string_view name
) {
    const auto& result = property(value, name);
    return result.isString() ? result.stringValue() : std::string();
}

double numberProperty(
    const JsRuntimeValue& value,
    std::string_view name
) {
    const auto& result = property(value, name);
    return result.isNumber() ? result.numberValue() : 0.0;
}

int64_t integerProperty(
    const JsRuntimeValue& value,
    std::string_view name
) {
    const auto& result = property(value, name);
    if (result.isNumber()) return static_cast<int64_t>(result.numberValue());
    if (result.isString()) {
        try { return std::stoll(result.stringValue()); } catch (...) {}
    }
    return 0;
}

bool boolProperty(
    const JsRuntimeValue& value,
    std::string_view name
) {
    const auto& result = property(value, name);
    return result.isBool() && result.boolValue();
}

std::string realmId(int64_t value) {
    return std::to_string(value);
}

std::string encodeURIComponent(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char byte : value) {
        const bool safe =
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '_' || byte == '.' || byte == '!' ||
            byte == '~' || byte == '*' || byte == '\'' || byte == '(' ||
            byte == ')';
        if (safe) {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
    }
    return result;
}

std::string cleanInviteCode(std::string value) {
    constexpr std::string_view prefix = "https://realms.gg/";
    std::size_t offset = 0;
    while ((offset = value.find(prefix, offset)) != std::string::npos) {
        value.erase(offset, prefix.size());
    }
    return value;
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return value;
}

BedrockRealmApi& requireApi(const std::shared_ptr<BedrockRealmApi>& api) {
    if (!api) throw std::runtime_error("Bedrock Realm API is not available");
    return *api;
}

} // namespace

std::shared_ptr<BedrockRealmApi> BedrockRealmApi::from(
    AuthflowPtr authflow,
    BedrockRealmsOptions options
) {
    return std::shared_ptr<BedrockRealmApi>(
        new BedrockRealmApi(std::move(authflow), std::move(options))
    );
}

BedrockRealmApi::BedrockRealmApi(
    AuthflowPtr authflow,
    BedrockRealmsOptions options
) : authflow_(std::move(authflow)), options_(std::move(options)) {
    microtaskQueue_ = options_.microtaskQueue
        ? options_.microtaskQueue
        : JsMicrotaskQueue::create();
    httpClient_ = options_.httpClient
        ? options_.httpClient
        : std::make_shared<CurlXboxTokenHttpClient>(microtaskQueue_);
    if (!options_.delay) {
        options_.delay = [](std::chrono::milliseconds duration) {
            std::this_thread::sleep_for(duration);
        };
    }
}

JsRuntimeValue BedrockRealmApi::request(
    std::string method,
    std::string route,
    JsRuntimeValue body
) {
    XboxTokenHttpRequest request;
    request.method = std::move(method);
    request.url = std::string(Host) + std::move(route);
    request.headers.emplace_back("Client-Version", "0.0.0");
    request.headers.emplace_back("User-Agent", UserAgent);
    if (options_.usePreview) {
        request.headers.emplace_back("is-prerelease", "true");
    }
    if (!options_.skipAuth) {
        if (!authflow_) {
            throw std::runtime_error("authflow.getXboxToken is not a function");
        }
        const auto token = authflow_->getXboxToken(RelyingParty, false).get();
        request.headers.emplace_back(
            "authorization",
            "XBL3.0 x=" + token.userHash + ";" + token.XSTSToken
        );
    }
    if (!body.isNull()) {
        request.headers.emplace_back("Content-Type", "application/json");
        if (const auto serialized = JsRuntimeJson::stringify(body)) {
            request.body = *serialized;
        }
    }
    for (const auto& [name, value] : request.headers) {
        request.headersObject.set(name, JsRuntimeValue::string(value));
    }
    return execute(request);
}

JsRuntimeValue BedrockRealmApi::execute(
    const XboxTokenHttpRequest& request,
    int retries
) {
    const auto response = httpClient_->fetch(request).get();
    if (response.ok()) {
        const auto* contentType = response.header("Content-Type");
        if (contentType && contentType->starts_with("application/json")) {
            return JsRuntimeJson::parse(response.bodyText);
        }
        return JsRuntimeValue::string(response.bodyText);
    }
    if (response.status >= 500 && response.status < 600 &&
        retries < options_.maxRetries) {
        const auto exponent = std::min(retries, 30);
        const auto delay = std::chrono::milliseconds(
            static_cast<int64_t>(1000) << exponent
        );
        options_.delay(delay);
        return execute(request, retries + 1);
    }
    throw std::runtime_error(
        std::to_string(response.status) + " " + response.statusText + " " +
        response.bodyText
    );
}

BedrockRealm BedrockRealmApi::makeRealm(const JsRuntimeValue& value) {
    BedrockRealm realm;
    realm.api_ = shared_from_this();
    realm.raw = value;
    if (!value.isObject()) return realm;
    realm.id = integerProperty(value, "id");
    realm.remoteSubscriptionId = stringProperty(value, "remoteSubscriptionId");
    realm.owner = stringProperty(value, "owner");
    realm.ownerUUID = stringProperty(value, "ownerUUID");
    realm.name = stringProperty(value, "name");
    realm.motd = stringProperty(value, "motd");
    realm.defaultPermission = stringProperty(value, "defaultPermission");
    realm.state = stringProperty(value, "state");
    realm.daysLeft = numberProperty(value, "daysLeft");
    realm.expired = boolProperty(value, "expired");
    realm.expiredTrial = boolProperty(value, "expiredTrial");
    realm.gracePeriod = boolProperty(value, "gracePeriod");
    realm.worldType = stringProperty(value, "worldType");
    realm.players = property(value, "players");
    realm.maxPlayers = integerProperty(value, "maxPlayers");
    realm.minigameName = property(value, "minigameName");
    realm.minigameId = property(value, "minigameId");
    realm.minigameImage = property(value, "minigameImage");
    realm.activeSlot = integerProperty(value, "activeSlot");
    realm.slots = property(value, "slots");
    realm.member = boolProperty(value, "member");
    realm.clubId = integerProperty(value, "clubId");
    realm.subscriptionRefreshStatus =
        property(value, "subscriptionRefreshStatus");
    return realm;
}

BedrockRealmBackup BedrockRealmApi::makeBackup(
    int64_t realmIdValue,
    int64_t slotId,
    const JsRuntimeValue& value
) {
    BedrockRealmBackup backup;
    backup.api_ = shared_from_this();
    backup.realmId_ = realmIdValue;
    backup.slotId_ = slotId;
    backup.id = stringProperty(value, "backupId");
    backup.lastModifiedDate = numberProperty(value, "lastModifiedDate");
    backup.size = numberProperty(value, "size");
    const auto& metadata = property(value, "metadata");
    backup.metadata.gameDifficulty = stringProperty(metadata, "game_difficulty");
    backup.metadata.name = stringProperty(metadata, "name");
    backup.metadata.gameServerVersion =
        stringProperty(metadata, "game_server_version");
    const auto enabledPacks = stringProperty(metadata, "enabled_packs");
    backup.metadata.enabledPacks = enabledPacks.empty()
        ? JsRuntimeValue::undefined()
        : JsRuntimeJson::parse(enabledPacks);
    backup.metadata.description = property(metadata, "description");
    backup.metadata.gamemode = stringProperty(metadata, "game_mode");
    backup.metadata.worldType = stringProperty(metadata, "world_type");
    return backup;
}

BedrockRealmDownload BedrockRealmApi::makeDownload(
    const JsRuntimeValue& value
) {
    BedrockRealmDownload download;
    download.api_ = shared_from_this();
    download.downloadUrl = stringProperty(value, "downloadLink");
    if (download.downloadUrl.empty()) {
        download.downloadUrl = stringProperty(value, "downloadUrl");
    }
    download.token = stringProperty(value, "token");
    download.size = numberProperty(value, "size");
    return download;
}

BedrockRealm BedrockRealmApi::getRealm(int64_t realmIdValue) {
    return makeRealm(request("get", "/worlds/" + realmId(realmIdValue)));
}

std::vector<BedrockRealm> BedrockRealmApi::getRealms() {
    const auto data = request("get", "/worlds");
    const auto& servers = property(data, "servers");
    std::vector<BedrockRealm> realms;
    if (!servers.isArray()) return realms;
    realms.reserve(servers.length());
    for (std::size_t index = 0; index < servers.length(); ++index) {
        if (const auto* value = servers.get(index)) {
            realms.push_back(makeRealm(*value));
        }
    }
    return realms;
}

std::vector<BedrockRealmBackup> BedrockRealmApi::getRealmBackups(
    int64_t realmIdValue,
    int64_t slotId
) {
    const auto data = request(
        "get",
        "/worlds/" + realmId(realmIdValue) + "/backups"
    );
    const auto& backupsValue = property(data, "backups");
    std::vector<BedrockRealmBackup> backups;
    if (!backupsValue.isArray()) return backups;
    backups.reserve(backupsValue.length());
    for (std::size_t index = 0; index < backupsValue.length(); ++index) {
        if (const auto* value = backupsValue.get(index)) {
            backups.push_back(makeBackup(realmIdValue, slotId, *value));
        }
    }
    return backups;
}

JsRuntimeValue BedrockRealmApi::restoreRealmFromBackup(
    int64_t realmIdValue,
    const std::string& backupId
) {
    return request(
        "put",
        "/worlds/" + realmId(realmIdValue) + "/backups?backupId=" +
            encodeURIComponent(backupId) + "&clientSupportsRetries"
    );
}

JsRuntimeValue BedrockRealmApi::getRealmSubscriptionInfo(
    int64_t realmIdValue,
    bool detailed
) {
    const auto data = request(
        "get",
        "/subscriptions/" + realmId(realmIdValue) +
            (detailed ? "/details" : "")
    );
    if (detailed) {
        return JsRuntimeValue::object({
            {"type", property(data, "type")},
            {"store", property(data, "store")},
            {"startDate", property(data, "startDate")},
            {"endDate", property(data, "endDate")},
            {"renewalPeriod", property(data, "renewalPeriod")},
            {"daysLeft", property(data, "daysLeft")},
            {"subscriptionId", property(data, "subscriptionId")}
        });
    }
    return JsRuntimeValue::object({
        {"startDate", property(data, "startDate")},
        {"daysLeft", property(data, "daysLeft")},
        {"subscriptionType", property(data, "subscriptionType")}
    });
}

JsRuntimeValue BedrockRealmApi::changeRealmState(
    int64_t realmIdValue,
    const std::string& state
) {
    return request(
        "put",
        "/worlds/" + realmId(realmIdValue) + "/" + state
    );
}

JsRuntimeValue BedrockRealmApi::changeRealmActiveSlot(
    int64_t realmIdValue,
    int64_t slotId
) {
    return request(
        "put",
        "/worlds/" + realmId(realmIdValue) + "/slot/" + realmId(slotId)
    );
}

void BedrockRealmApi::changeRealmNameAndDescription(
    int64_t realmIdValue,
    const std::string& name,
    const std::string& description
) {
    const auto route = "/worlds/" + realmId(realmIdValue);
    if (!name.empty() && !description.empty()) {
        (void) request("post", route, JsRuntimeValue::object({
            {"name", JsRuntimeValue::string(name)},
            {"description", JsRuntimeValue::string("")}
        }));
    }
    (void) request("post", route, JsRuntimeValue::object({
        {"name", JsRuntimeValue::string(name)},
        {"description", JsRuntimeValue::string(description)}
    }));
}

void BedrockRealmApi::deleteRealm(int64_t realmIdValue) {
    (void) request("delete", "/worlds/" + realmId(realmIdValue));
}

BedrockRealmAddress BedrockRealmApi::getRealmAddress(int64_t realmIdValue) {
    const auto data = request(
        "get",
        "/worlds/" + realmId(realmIdValue) + "/join"
    );
    const auto address = stringProperty(data, "address");
    const auto separator = address.find(':');
    BedrockRealmAddress result;
    result.host = address.substr(0, separator);
    if (separator != std::string::npos) {
        const auto next = address.find(':', separator + 1);
        const auto port = address.substr(
            separator + 1,
            next == std::string::npos
                ? std::string::npos
                : next - separator - 1
        );
        try {
            result.port = static_cast<uint16_t>(std::stoul(port));
        } catch (...) {
            result.port = 0;
        }
    }
    return result;
}

BedrockRealm BedrockRealmApi::getRealmFromInvite(
    const std::string& realmInviteCode,
    bool invite
) {
    if (realmInviteCode.empty()) {
        throw std::runtime_error("Need to provide a realm invite code/link");
    }
    const auto data = request(
        "get",
        "/worlds/v1/link/" + cleanInviteCode(realmInviteCode)
    );
    if (!property(data, "member").truthy() && invite) {
        (void) acceptRealmInviteFromCode(realmInviteCode);
    }
    return makeRealm(data);
}

BedrockRealmInvite BedrockRealmApi::getRealmInvite(int64_t realmIdValue) {
    const auto data = request(
        "get",
        "/links/v1?worldId=" + realmId(realmIdValue)
    );
    const auto* first = data.isArray() ? data.get(0) : nullptr;
    if (!first) throw std::runtime_error("Realm invite response is empty");
    return BedrockRealmInvite {
        .inviteCode = stringProperty(*first, "linkId"),
        .ownerXUID = stringProperty(*first, "profileUuid"),
        .type = stringProperty(*first, "type"),
        .createdOn = numberProperty(*first, "ts"),
        .inviteLink = stringProperty(*first, "url"),
        .deepLinkUrl = stringProperty(*first, "deepLinkUrl")
    };
}

BedrockRealmInvite BedrockRealmApi::refreshRealmInvite(int64_t realmIdValue) {
    const auto data = request("post", "/links/v1", JsRuntimeValue::object({
        {"type", JsRuntimeValue::string("INFINITE")},
        {"worldId", JsRuntimeValue::number(static_cast<double>(realmIdValue))}
    }));
    return BedrockRealmInvite {
        .inviteCode = stringProperty(data, "linkId"),
        .ownerXUID = stringProperty(data, "profileUuid"),
        .type = stringProperty(data, "type"),
        .createdOn = numberProperty(data, "ts"),
        .inviteLink = stringProperty(data, "url"),
        .deepLinkUrl = stringProperty(data, "deepLinkUrl")
    };
}

JsRuntimeValue BedrockRealmApi::getPendingInviteCount() {
    return request("get", "/invites/count/pending");
}

std::vector<BedrockPendingRealmInvite> BedrockRealmApi::getPendingInvites() {
    const auto data = request("get", "/invites/pending");
    const auto& values = property(data, "invites");
    std::vector<BedrockPendingRealmInvite> invites;
    if (!values.isArray()) return invites;
    invites.reserve(values.length());
    for (std::size_t index = 0; index < values.length(); ++index) {
        const auto* value = values.get(index);
        if (!value) continue;
        invites.push_back(BedrockPendingRealmInvite {
            .invitationId = stringProperty(*value, "invitationId"),
            .worldName = stringProperty(*value, "worldName"),
            .worldDescription = stringProperty(*value, "worldDescription"),
            .worldOwnerName = property(*value, "worldOwnerName"),
            .worldOwnerXUID = stringProperty(*value, "worldOwnerUuid"),
            .createdOn = numberProperty(*value, "date")
        });
    }
    return invites;
}

void BedrockRealmApi::acceptRealmInvitation(const std::string& invitationId) {
    (void) request("put", "/invites/accept/" + invitationId);
}

void BedrockRealmApi::rejectRealmInvitation(const std::string& invitationId) {
    (void) request("put", "/invites/reject/" + invitationId);
}

BedrockRealm BedrockRealmApi::acceptRealmInviteFromCode(
    const std::string& inviteCode
) {
    if (inviteCode.empty()) {
        throw std::runtime_error("Need to provide a realm invite code/link");
    }
    return makeRealm(request(
        "post",
        "/invites/v1/link/accept/" + cleanInviteCode(inviteCode)
    ));
}

BedrockRealm BedrockRealmApi::invitePlayer(
    int64_t realmIdValue,
    const std::string& uuid
) {
    return makeRealm(request(
        "put",
        "/invites/" + realmId(realmIdValue) + "/invite/update",
        JsRuntimeValue::object({
            {"invites", JsRuntimeValue::object({
                {uuid, JsRuntimeValue::string("ADD")}
            })}
        })
    ));
}

BedrockRealmDownload BedrockRealmApi::getRealmWorldDownload(
    int64_t realmIdValue,
    int64_t slotId,
    const std::string& backupId
) {
    return makeDownload(request(
        "get",
        "/archive/download/world/" + realmId(realmIdValue) + "/" +
            realmId(slotId) + "/" + backupId
    ));
}

void BedrockRealmApi::resetRealm(int64_t realmIdValue) {
    (void) request("put", "/worlds/" + realmId(realmIdValue) + "/reset");
}

BedrockRealm BedrockRealmApi::removePlayerFromRealm(
    int64_t realmIdValue,
    const std::string& xuid
) {
    return makeRealm(request(
        "put",
        "/invites/" + realmId(realmIdValue) + "/invite/update",
        JsRuntimeValue::object({
            {"invites", JsRuntimeValue::object({
                {xuid, JsRuntimeValue::string("REMOVE")}
            })}
        })
    ));
}

BedrockRealm BedrockRealmApi::opRealmPlayer(
    int64_t realmIdValue,
    const std::string& uuid
) {
    return makeRealm(request(
        "put",
        "/invites/" + realmId(realmIdValue) + "/invite/update",
        JsRuntimeValue::object({
            {"invites", JsRuntimeValue::object({
                {uuid, JsRuntimeValue::string("OP")}
            })}
        })
    ));
}

BedrockRealm BedrockRealmApi::deopRealmPlayer(
    int64_t realmIdValue,
    const std::string& uuid
) {
    return makeRealm(request(
        "put",
        "/invites/" + realmId(realmIdValue) + "/invite/update",
        JsRuntimeValue::object({
            {"invites", JsRuntimeValue::object({
                {uuid, JsRuntimeValue::string("DEOP")}
            })}
        })
    ));
}

void BedrockRealmApi::banPlayerFromRealm(
    int64_t realmIdValue,
    const std::string& uuid
) {
    (void) request(
        "post",
        "/worlds/" + realmId(realmIdValue) + "/blocklist/" + uuid
    );
}

void BedrockRealmApi::unbanPlayerFromRealm(
    int64_t realmIdValue,
    const std::string& uuid
) {
    (void) request(
        "delete",
        "/worlds/" + realmId(realmIdValue) + "/blocklist/" + uuid
    );
}

void BedrockRealmApi::removeRealmFromJoinedList(int64_t realmIdValue) {
    (void) request("delete", "/invites/" + realmId(realmIdValue));
}

void BedrockRealmApi::changeIsTexturePackRequired(
    int64_t realmIdValue,
    bool forced
) {
    (void) request(
        forced ? "put" : "delete",
        "/world/" + realmId(realmIdValue) +
            "/content/texturePacksRequired"
    );
}

void BedrockRealmApi::changeRealmDefaultPermission(
    int64_t realmIdValue,
    std::string permission
) {
    (void) request(
        "put",
        "/world/" + realmId(realmIdValue) + "/defaultPermission",
        JsRuntimeValue::object({
            {"permission", JsRuntimeValue::string(upperAscii(
                std::move(permission)
            ))}
        })
    );
}

void BedrockRealmApi::changeRealmPlayerPermission(
    int64_t realmIdValue,
    std::string permission,
    const std::string& uuid
) {
    (void) request(
        "put",
        "/world/" + realmId(realmIdValue) + "/userPermission",
        JsRuntimeValue::object({
            {"permission", JsRuntimeValue::string(upperAscii(
                std::move(permission)
            ))},
            {"xuid", JsRuntimeValue::string(uuid)}
        })
    );
}

std::vector<uint8_t> BedrockRealmApi::downloadWorld(
    const BedrockRealmDownload& value
) {
    XboxTokenHttpRequest request;
    request.method = "get";
    request.url = value.downloadUrl;
    if (!value.token.empty()) {
        request.headers.emplace_back(
            "Authorization",
            "Bearer " + value.token
        );
        request.headersObject.set(
            "Authorization",
            JsRuntimeValue::string("Bearer " + value.token)
        );
    }
    const auto response = httpClient_->fetch(std::move(request)).get();
    if (!response.ok()) {
        throw std::runtime_error(
            "Failed to download world: " +
            std::to_string(response.status) + " " + response.statusText
        );
    }
    return std::vector<uint8_t>(
        response.bodyText.begin(),
        response.bodyText.end()
    );
}

BedrockRealmAddress BedrockRealm::getAddress() const {
    return requireApi(api_).getRealmAddress(id);
}

BedrockRealm BedrockRealm::invitePlayer(const std::string& uuid) const {
    return requireApi(api_).invitePlayer(id, uuid);
}

JsRuntimeValue BedrockRealm::open() const {
    return requireApi(api_).changeRealmState(id, "open");
}

JsRuntimeValue BedrockRealm::close() const {
    return requireApi(api_).changeRealmState(id, "close");
}

void BedrockRealm::deleteRealm() const {
    requireApi(api_).deleteRealm(id);
}

BedrockRealmDownload BedrockRealm::getWorldDownload() const {
    return requireApi(api_).getRealmWorldDownload(id, activeSlot, "latest");
}

std::vector<BedrockRealmBackup> BedrockRealm::getBackups() const {
    return requireApi(api_).getRealmBackups(id, activeSlot);
}

JsRuntimeValue BedrockRealm::getSubscriptionInfo(bool detailed) const {
    return requireApi(api_).getRealmSubscriptionInfo(id, detailed);
}

JsRuntimeValue BedrockRealm::changeActiveSlot(int64_t slotId) const {
    return requireApi(api_).changeRealmActiveSlot(id, slotId);
}

void BedrockRealm::changeNameAndDescription(
    const std::string& name,
    const std::string& description
) const {
    requireApi(api_).changeRealmNameAndDescription(id, name, description);
}

BedrockRealmDownload BedrockRealmBackup::getDownload() const {
    return requireApi(api_).getRealmWorldDownload(
        realmId_,
        slotId_,
        id
    );
}

JsRuntimeValue BedrockRealmBackup::restore() const {
    return requireApi(api_).restoreRealmFromBackup(realmId_, id);
}

std::vector<uint8_t> BedrockRealmDownload::getBuffer() const {
    return requireApi(api_).downloadWorld(*this);
}

void BedrockRealmDownload::writeToDirectory(
    const std::filesystem::path& directory
) const {
    const auto bytes = getBuffer();
    std::ofstream output(
        directory / ("world" + fileExtension),
        std::ios::binary | std::ios::trunc
    );
    if (!output) {
        throw std::runtime_error("Failed to open Realm world output file");
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!output) {
        throw std::runtime_error("Failed to write Realm world output file");
    }
}

BedrockRealmAddress resolveBedrockRealmAddress(
    AuthflowPtr authflow,
    const BedrockRealmSelection& selection
) {
    if (selection.addressResolver) {
        return selection.addressResolver(std::move(authflow));
    }

    auto api = BedrockRealmApi::from(
        std::move(authflow),
        BedrockRealmsOptions {
            .usePreview = selection.usePreview,
            .maxRetries = selection.maxRetries
        }
    );
    std::optional<BedrockRealm> realm;
    const auto getRealms = [&]() {
        auto realms = api->getRealms();
        if (realms.empty()) {
            throw std::runtime_error(
                "Couldn't find any Realms for the authenticated account"
            );
        }
        return realms;
    };

    if (selection.realmId.has_value()) {
        const auto realms = getRealms();
        const auto found = std::find_if(
            realms.begin(),
            realms.end(),
            [&](const BedrockRealm& candidate) {
                return candidate.id == *selection.realmId;
            }
        );
        if (found != realms.end()) realm = *found;
    } else if (!selection.realmInvite.empty()) {
        realm = api->getRealmFromInvite(selection.realmInvite);
    } else if (selection.pickRealm) {
        realm = selection.pickRealm(getRealms());
    } else if (selection.pickRealmAsync) {
        realm = selection.pickRealmAsync(getRealms()).get();
    }

    if (!realm.has_value()) {
        throw std::runtime_error(
            "Couldn't find a Realm to connect to. Authenticated account "
            "must be the owner or has been invited to the Realm."
        );
    }
    return api->getRealmAddress(realm->id);
}

} // namespace bedrock
