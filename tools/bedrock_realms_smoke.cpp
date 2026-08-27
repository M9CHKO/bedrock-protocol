#include <bedrock/realms/BedrockRealms.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

bool check(bool condition, const std::string& message) {
    if (!condition) std::cerr << "[BEDROCK-REALMS-SMOKE] " << message << "\n";
    return condition;
}

class FakeHttp final : public bedrock::IXboxTokenHttpClient {
public:
    explicit FakeHttp(std::shared_ptr<bedrock::JsMicrotaskQueue> queue)
        : queue_(std::move(queue)) {}

    bedrock::JsPromise<bedrock::XboxTokenHttpResponse> fetch(
        bedrock::XboxTokenHttpRequest request
    ) override {
        requests.push_back(std::move(request));
        if (responses.empty()) {
            return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::rejected(
                queue_,
                "Fake Realm HTTP response queue exhausted"
            );
        }
        auto response = std::move(responses.front());
        responses.pop_front();
        return bedrock::JsPromise<bedrock::XboxTokenHttpResponse>::resolved(
            queue_,
            std::move(response)
        );
    }

    std::deque<bedrock::XboxTokenHttpResponse> responses;
    std::vector<bedrock::XboxTokenHttpRequest> requests;

private:
    std::shared_ptr<bedrock::JsMicrotaskQueue> queue_;
};

bedrock::XboxTokenHttpResponse json(
    int status,
    std::string body,
    std::string statusText = "OK"
) {
    return bedrock::XboxTokenHttpResponse {
        .status = status,
        .statusText = std::move(statusText),
        .headers = {{"Content-Type", "application/json; charset=utf-8"}},
        .bodyText = std::move(body)
    };
}

bedrock::XboxTokenHttpResponse text(
    int status,
    std::string body,
    std::string statusText = "OK"
) {
    return bedrock::XboxTokenHttpResponse {
        .status = status,
        .statusText = std::move(statusText),
        .bodyText = std::move(body)
    };
}

std::shared_ptr<bedrock::Authflow> authflow(int& calls) {
    return std::make_shared<bedrock::Authflow>(
        bedrock::Authflow::MinecraftBedrockTokenMethod {},
        [&calls](std::string relyingParty, bool forceRefresh) {
            ++calls;
            if (relyingParty != bedrock::BedrockRealmApi::RelyingParty ||
                forceRefresh) {
                return bedrock::makeRejectedAuthflowFuture<bedrock::XboxToken>(
                    "unexpected Realms getXboxToken arguments"
                );
            }
            return bedrock::makeReadyAuthflowFuture(bedrock::XboxToken {
                .userXUID = "42",
                .userHash = "HASH",
                .XSTSToken = "XSTS",
                .expiresOn = "2030-01-01T00:00:00Z"
            });
        }
    );
}

bool checkCoreAndStructures() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto http = std::make_shared<FakeHttp>(queue);
    int authCalls = 0;
    auto api = bedrock::BedrockRealmApi::from(
        authflow(authCalls),
        bedrock::BedrockRealmsOptions {
            .usePreview = true,
            .httpClient = http,
            .microtaskQueue = queue
        }
    );

    http->responses.push_back(json(200, R"({"servers":[{"id":1112223,"remoteSubscriptionId":"sub","owner":"Owner","ownerUUID":"42","name":"Test Realm","motd":"Description","defaultPermission":"VISITOR","state":"OPEN","daysLeft":2,"expired":false,"expiredTrial":false,"gracePeriod":false,"worldType":"NORMAL","players":null,"maxPlayers":10,"minigameName":null,"minigameId":null,"minigameImage":null,"activeSlot":1,"slots":null,"member":true,"clubId":99,"subscriptionRefreshStatus":null}]})"));
    const auto realms = api->getRealms();
    ok &= check(
        realms.size() == 1 && realms[0].id == 1112223 &&
            realms[0].name == "Test Realm" && realms[0].activeSlot == 1 &&
            realms[0].players.isNull(),
        "Node World fixture was not mapped to BedrockRealm"
    );
    const auto& worldsRequest = http->requests.back();
    ok &= check(
        worldsRequest.method == "get" &&
            worldsRequest.url ==
                "https://pocket.realms.minecraft.net/worlds" &&
            worldsRequest.body.empty() &&
            worldsRequest.header("Client-Version") &&
            *worldsRequest.header("Client-Version") == "0.0.0" &&
            worldsRequest.header("User-Agent") &&
            *worldsRequest.header("User-Agent") == "MCPE/UWP" &&
            worldsRequest.header("is-prerelease") &&
            *worldsRequest.header("is-prerelease") == "true" &&
            worldsRequest.header("authorization") &&
            *worldsRequest.header("authorization") ==
                "XBL3.0 x=HASH;XSTS" &&
            worldsRequest.header("Content-Type") &&
            *worldsRequest.header("Content-Type") == "application/json",
        "Realms REST headers differ from prismarine-realms"
    );

    http->responses.push_back(json(200, R"({"address":"0.0.0.0:19132","pendingUpdate":false})"));
    const auto address = realms[0].getAddress();
    ok &= check(
        address.host == "0.0.0.0" && address.port == 19132 &&
            http->requests.back().url.ends_with("/worlds/1112223/join"),
        "Realm.getAddress did not match the Node split/address endpoint"
    );

    http->responses.push_back(json(200, R"({"id":1112223,"member":false,"activeSlot":1})"));
    http->responses.push_back(json(200, R"({"id":1112223,"member":true})"));
    const auto invited = api->getRealmFromInvite(
        "https://realms.gg/AB1CD2EFA3B"
    );
    ok &= check(
        invited.id == 1112223 &&
            http->requests[2].url.ends_with("/worlds/v1/link/AB1CD2EFA3B") &&
            http->requests[3].url.ends_with(
                "/invites/v1/link/accept/AB1CD2EFA3B"
            ),
        "invite-link cleanup or automatic acceptance differs from Node"
    );

    http->responses.push_back(json(200, R"([{"linkId":"AB1CD2EFA3B","profileUuid":"42","type":"INFINITE","ts":1652129181690,"url":"https://realms.gg/AB1CD2EFA3B","deepLinkUrl":"minecraft://acceptRealmInvite?inviteID=AB1CD2EFA3B"}])"));
    const auto invite = api->getRealmInvite(1112223);
    ok &= check(
        invite.inviteCode == "AB1CD2EFA3B" && invite.ownerXUID == "42" &&
            invite.createdOn == 1652129181690.0,
        "Realm invite projection differs from prismarine-realms"
    );

    http->responses.push_back(json(200, R"({"backups":[{"backupId":"1970-01-01T00:00:00.000Z","lastModifiedDate":1652129181690,"size":10000,"metadata":{"game_difficulty":"0","name":"Test Realm","game_server_version":"1.19.0","enabled_packs":"{\"resourcePacks\":[],\"behaviorPacks\":[\"pack\"]}","description":null,"game_mode":"0","world_type":"NORMAL"}}]})"));
    const auto backups = api->getRealmBackups(1112223, 1);
    ok &= check(
        backups.size() == 1 &&
            backups[0].id == "1970-01-01T00:00:00.000Z" &&
            backups[0].metadata.enabledPacks.isObject() &&
            backups[0].metadata.description.isNull(),
        "Backup metadata/embedded enabled_packs JSON was not mapped"
    );

    http->responses.push_back(text(204, "", "No Content"));
    (void) backups[0].restore();
    ok &= check(
        http->requests.back().url.ends_with(
            "/worlds/1112223/backups?backupId="
            "1970-01-01T00%3A00%3A00.000Z&clientSupportsRetries"
        ),
        "backupId does not use encodeURIComponent-compatible routing"
    );

    http->responses.push_back(json(200, R"({"downloadUrl":"https://archive.example/world","token":"TOKEN","size":4})"));
    const auto download = backups[0].getDownload();
    ok &= check(
        download.downloadUrl == "https://archive.example/world" &&
            download.token == "TOKEN" &&
            http->requests.back().url.ends_with(
                "/archive/download/world/1112223/1/1970-01-01T00:00:00.000Z"
            ),
        "individual Bedrock backup download route differs from Node"
    );
    http->responses.push_back(text(200, std::string("\0A\xffZ", 4)));
    const auto bytes = download.getBuffer();
    ok &= check(
        bytes == std::vector<uint8_t>({0, 'A', 0xff, 'Z'}) &&
            http->requests.back().header("Authorization") &&
            *http->requests.back().header("Authorization") == "Bearer TOKEN",
        "Bedrock world download lost bytes or bearer token"
    );
    ok &= check(authCalls == 8, "REST requests did not authenticate once each");
    return ok;
}

bool checkMutationRoutes() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto http = std::make_shared<FakeHttp>(queue);
    auto api = bedrock::BedrockRealmApi::from(
        {},
        bedrock::BedrockRealmsOptions {
            .skipAuth = true,
            .httpClient = http,
            .microtaskQueue = queue
        }
    );

    http->responses.push_back(json(200, R"({"id":7})"));
    (void) api->invitePlayer(7, "uuid");
    ok &= check(
        http->requests.back().method == "put" &&
            http->requests.back().body ==
                R"({"invites":{"uuid":"ADD"}})",
        "invitePlayer body differs from Node JSON.stringify"
    );

    http->responses.push_back(text(204, "", "No Content"));
    http->responses.push_back(text(204, "", "No Content"));
    api->changeRealmNameAndDescription(7, "Hello", "World!");
    ok &= check(
        http->requests[1].method == "post" &&
            http->requests[1].body ==
                R"({"name":"Hello","description":""})" &&
            http->requests[2].body ==
                R"({"name":"Hello","description":"World!"})",
        "name/description two-request Realms workaround was not preserved"
    );

    http->responses.push_back(text(204, "", "No Content"));
    api->changeRealmDefaultPermission(7, "member");
    ok &= check(
        http->requests.back().url.ends_with(
            "/world/7/defaultPermission"
        ) && http->requests.back().body ==
            R"({"permission":"MEMBER"})",
        "default permission route/body differs from Bedrock API"
    );

    http->responses.push_back(text(204, "", "No Content"));
    api->changeRealmPlayerPermission(7, "visitor", "42");
    ok &= check(
        http->requests.back().body ==
            R"({"permission":"VISITOR","xuid":"42"})",
        "player permission route/body differs from Bedrock API"
    );

    http->responses.push_back(json(200, R"({"invites":[{"invitationId":"11","worldName":"Realm","worldDescription":"Motd","worldOwnerName":null,"worldOwnerUuid":"42","date":123}]})"));
    const auto pending = api->getPendingInvites();
    ok &= check(
        pending.size() == 1 && pending[0].invitationId == "11" &&
            pending[0].worldOwnerName.isNull() &&
            pending[0].worldOwnerXUID == "42",
        "pending invite projection differs from Node"
    );
    return ok;
}

bool checkEndpointMatrix() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto http = std::make_shared<FakeHttp>(queue);
    auto api = bedrock::BedrockRealmApi::from(
        {},
        bedrock::BedrockRealmsOptions {
            .skipAuth = true,
            .httpClient = http,
            .microtaskQueue = queue
        }
    );
    const auto verify = [&](std::string method, std::string suffix) {
        return check(
            http->requests.back().method == method &&
                http->requests.back().url.ends_with(suffix),
            method + " " + suffix + " route differs from prismarine-realms"
        );
    };

    http->responses.push_back(json(200, R"({"id":7})"));
    ok &= check(api->getRealm(7).id == 7, "getRealm did not map its response");
    ok &= verify("get", "/worlds/7");

    http->responses.push_back(json(200, R"({"linkId":"CODE","profileUuid":"42","type":"INFINITE","ts":1,"url":"url","deepLinkUrl":"deep"})"));
    ok &= check(
        api->refreshRealmInvite(7).inviteCode == "CODE",
        "refreshRealmInvite did not map its response"
    );
    ok &= verify("post", "/links/v1");
    ok &= check(
        http->requests.back().body ==
            R"({"type":"INFINITE","worldId":7})",
        "refreshRealmInvite body differs from Node"
    );

    http->responses.push_back(json(200, "3"));
    const auto pendingCount = api->getPendingInviteCount();
    ok &= check(
        pendingCount.isNumber() && pendingCount.numberValue() == 3.0,
        "getPendingInviteCount did not preserve the raw count"
    );
    ok &= verify("get", "/invites/count/pending");

    http->responses.push_back(text(204, "", "No Content"));
    api->acceptRealmInvitation("11");
    ok &= verify("put", "/invites/accept/11");

    http->responses.push_back(text(204, "", "No Content"));
    api->rejectRealmInvitation("12");
    ok &= verify("put", "/invites/reject/12");

    http->responses.push_back(json(200, R"({"id":7})"));
    ok &= check(
        api->acceptRealmInviteFromCode("https://realms.gg/CODE").id == 7,
        "acceptRealmInviteFromCode did not map its Realm"
    );
    ok &= verify("post", "/invites/v1/link/accept/CODE");

    http->responses.push_back(json(200, R"({"startDate":1,"daysLeft":2,"subscriptionType":"RECURRING","ignored":true})"));
    const auto basicSubscription = api->getRealmSubscriptionInfo(7);
    ok &= verify("get", "/subscriptions/7");
    ok &= check(
        bedrock::JsRuntimeJson::stringify(basicSubscription) ==
            std::optional<std::string>(
                R"({"startDate":1,"daysLeft":2,"subscriptionType":"RECURRING"})"
            ),
        "basic subscription projection differs from Node"
    );

    http->responses.push_back(json(200, R"({"type":"SUBSCRIPTION","store":"xbox1.store","startDate":1,"endDate":2,"renewalPeriod":30,"daysLeft":3,"subscriptionId":"sub","ignored":true})"));
    const auto detailedSubscription = api->getRealmSubscriptionInfo(7, true);
    ok &= verify("get", "/subscriptions/7/details");
    ok &= check(
        bedrock::JsRuntimeJson::stringify(detailedSubscription) ==
            std::optional<std::string>(
                R"({"type":"SUBSCRIPTION","store":"xbox1.store","startDate":1,"endDate":2,"renewalPeriod":30,"daysLeft":3,"subscriptionId":"sub"})"
            ),
        "detailed subscription projection differs from Node"
    );

    http->responses.push_back(json(200, "true"));
    ok &= check(
        api->changeRealmState(7, "open").truthy(),
        "changeRealmState did not return the endpoint value"
    );
    ok &= verify("put", "/worlds/7/open");

    http->responses.push_back(json(200, "true"));
    ok &= check(
        api->changeRealmActiveSlot(7, 2).truthy(),
        "changeRealmActiveSlot did not return the endpoint value"
    );
    ok &= verify("put", "/worlds/7/slot/2");

    http->responses.push_back(text(204, "", "No Content"));
    api->deleteRealm(7);
    ok &= verify("delete", "/worlds/7");

    http->responses.push_back(json(200, R"({"downloadLink":"https://archive.example/latest","token":"T","size":1})"));
    ok &= check(
        api->getRealmWorldDownload(7, 2).downloadUrl ==
            "https://archive.example/latest",
        "latest Realm world download did not map downloadLink"
    );
    ok &= verify("get", "/archive/download/world/7/2/latest");

    http->responses.push_back(json(200, "true"));
    api->resetRealm(7);
    ok &= verify("put", "/worlds/7/reset");

    const auto realmMutation = [&](
        const std::string& operation,
        const std::function<bedrock::BedrockRealm()>& invoke,
        const std::string& expectedBody
    ) {
        http->responses.push_back(json(200, R"({"id":7})"));
        const auto result = invoke();
        ok &= check(
            result.id == 7 && http->requests.back().body == expectedBody,
            operation + " Realm/body differs from Node"
        );
        ok &= verify("put", "/invites/7/invite/update");
    };
    realmMutation(
        "removePlayerFromRealm",
        [&]() { return api->removePlayerFromRealm(7, "42"); },
        R"({"invites":{"42":"REMOVE"}})"
    );
    realmMutation(
        "opRealmPlayer",
        [&]() { return api->opRealmPlayer(7, "42"); },
        R"({"invites":{"42":"OP"}})"
    );
    realmMutation(
        "deopRealmPlayer",
        [&]() { return api->deopRealmPlayer(7, "42"); },
        R"({"invites":{"42":"DEOP"}})"
    );

    http->responses.push_back(text(204, "", "No Content"));
    api->banPlayerFromRealm(7, "42");
    ok &= verify("post", "/worlds/7/blocklist/42");

    http->responses.push_back(text(204, "", "No Content"));
    api->unbanPlayerFromRealm(7, "42");
    ok &= verify("delete", "/worlds/7/blocklist/42");

    http->responses.push_back(text(204, "", "No Content"));
    api->removeRealmFromJoinedList(7);
    ok &= verify("delete", "/invites/7");

    http->responses.push_back(text(204, "", "No Content"));
    api->changeIsTexturePackRequired(7, true);
    ok &= verify("put", "/world/7/content/texturePacksRequired");

    http->responses.push_back(text(204, "", "No Content"));
    api->changeIsTexturePackRequired(7, false);
    ok &= verify("delete", "/world/7/content/texturePacksRequired");

    bedrock::BedrockRealmSelection selection;
    selection.realmId = "1112223";
    ok &= check(
        static_cast<bool>(selection) && *selection.realmId == 1112223,
        "decimal string Realm id was not accepted"
    );
    selection = false;
    ok &= check(
        !static_cast<bool>(selection) && !selection.realmId.has_value(),
        "assigning false did not clear the Realm selector"
    );
    return ok;
}

bool checkRetryAndErrors() {
    bool ok = true;
    auto queue = bedrock::JsMicrotaskQueue::create();
    auto http = std::make_shared<FakeHttp>(queue);
    std::vector<std::chrono::milliseconds> delays;
    auto api = bedrock::BedrockRealmApi::from(
        {},
        bedrock::BedrockRealmsOptions {
            .skipAuth = true,
            .maxRetries = 2,
            .httpClient = http,
            .microtaskQueue = queue,
            .delay = [&](std::chrono::milliseconds value) {
                delays.push_back(value);
            }
        }
    );
    http->responses.push_back(text(503, "one", "Service Unavailable"));
    http->responses.push_back(text(500, "two", "Internal Server Error"));
    http->responses.push_back(json(200, R"({"servers":[]})"));
    ok &= check(
        api->getRealms().empty() &&
            delays == std::vector<std::chrono::milliseconds>({
                std::chrono::milliseconds(1000),
                std::chrono::milliseconds(2000)
            }) && http->requests.size() == 3,
        "5xx exponential retry/maxRetries behavior differs from Node"
    );

    http->responses.push_back(text(429, "slow down", "Too Many Requests"));
    std::string message;
    try {
        (void) api->getRealm(7);
    } catch (const std::exception& error) {
        message = error.what();
    }
    ok &= check(
        message == "429 Too Many Requests slow down",
        "non-retry Realms error text differs from Node"
    );

    bool emptyInvite = false;
    try {
        (void) api->getRealmFromInvite("");
    } catch (const std::exception& error) {
        emptyInvite = std::string(error.what()) ==
            "Need to provide a realm invite code/link";
    }
    ok &= check(emptyInvite, "empty invite validation text differs from Node");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= checkCoreAndStructures();
    ok &= checkMutationRoutes();
    ok &= checkEndpointMatrix();
    ok &= checkRetryAndErrors();
    if (ok) std::cout << "[BEDROCK-REALMS-SMOKE] ok\n";
    return ok ? 0 : 1;
}
