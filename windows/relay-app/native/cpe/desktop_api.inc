// Included into the shared relay implementation. The Windows UI uses a small
// UTF-8 C ABI, while packet/NBT/deposit behavior is the Android implementation.
namespace {
using DesktopValue = bedrock::JsRuntimeValue;
std::mutex desktopApiMutex;

bool desktopBool(const DesktopValue& value, const char* name, bool fallback = false) {
    const auto* item = value.get(name);
    return item && item->isBool() ? item->boolValue() : fallback;
}
int desktopInt(const DesktopValue& value, const char* name, int fallback) {
    const auto* item = value.get(name);
    if (!item || !item->isNumber()) return fallback;
    const double number = item->numberValue();
    if (!std::isfinite(number) || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        throw std::runtime_error("Invalid integer setting");
    return static_cast<int>(number);
}
void desktopStop() {
    std::shared_ptr<RelayController> previous;
    { std::lock_guard lock(controllerMutex); previous = std::move(controller); }
    if (previous) previous->stop();
    previous.reset();
    { std::lock_guard lock(controllerMutex); currentState.reset(); }
}
DesktopValue desktopCall(const DesktopValue& request) {
    const auto action = stringProperty(request, "action");
    if (action == "versions") {
        auto result = DesktopValue::array();
        for (const auto& version : bedrock::versions()) result.push(DesktopValue::string(version));
        return result;
    }
    if (action == "stop") { desktopStop(); return DesktopValue::object({{"ok", DesktopValue::boolean(true)}}); }
    if (action == "start") {
        const auto host = stringProperty(request, "host");
        const auto version = stringProperty(request, "version");
        const auto root = stringProperty(request, "directory");
        const auto nbt = stringProperty(request, "nbtDirectory");
        const auto data = stringProperty(request, "minecraftDataDirectory");
        const auto authProfile = stringProperty(request, "authProfile");
        const int port = desktopInt(request, "port", 19132);
        if (host.empty() || host.size() > 253 || host.find_first_of("\r\n /\\") != std::string::npos)
            throw std::runtime_error("Введите адрес сервера без пробелов и https://");
        if (port < 1 || port > 65535) throw std::runtime_error("Порт должен быть 1–65535");
        if (!bedrock::supportsVersion(version)) throw std::runtime_error("Неподдерживаемая версия Minecraft");
        if (root.empty() || nbt.empty()) throw std::runtime_error("Не выбрана папка приложения/NBT");
        if (!authProfile.empty() && (authProfile.size() > 32 || authProfile.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _-") != std::string::npos || authProfile.back() == ' '))
            throw std::runtime_error("Недопустимое имя профиля");
        const auto directory = std::filesystem::u8path(root);
        if (!directory.is_absolute() || !std::filesystem::u8path(nbt).is_absolute())
            throw std::runtime_error("Нужны абсолютные пути к папкам");
        desktopStop();
        auto state = std::make_shared<RelayState>();
        state->destinationHost = host;
        state->destinationPort = static_cast<uint16_t>(port);
        state->version = version;
        state->shulkerDeposit.setVersion(version);
        state->nbtTransferDirectory = std::filesystem::u8path(nbt);
        std::filesystem::create_directories(state->nbtTransferDirectory);
        const auto authDirectory = authProfile.empty() ? directory / "auth" : directory / "profiles" / authProfile / "auth";
        std::filesystem::create_directories(authDirectory);
        state->configureRuntime(configuredDetailedLogging.load(), false, 24);
        state->configureGameplayFeatures(configuredAutoArmor.load(), configuredAutoTotem.load(), false, false);
        state->configureDeposit(configuredShulkerDeposit.load(), configuredShulkerDepositHotbar.load(), configuredShulkerDepositIntervalMs.load());
        state->relayStartedAt = unixMilliseconds();
        if (!data.empty()) state->loadBlockRegistry(std::filesystem::u8path(data));
        { std::lock_guard lock(controllerMutex); currentState = state; }
        auto next = std::make_shared<RelayController>(state);
        try {
            state->running = true;
            next->start(host, static_cast<uint16_t>(port), version, authDirectory, authProfile);
            { std::lock_guard lock(controllerMutex); controller = std::move(next); }
        } catch (...) {
            next->stop();
            { std::lock_guard lock(state->mutex); state->running = false; state->listening = false; }
            throw;
        }
        return snapshotValue(state, true);
    }
    std::shared_ptr<RelayState> state;
    { std::lock_guard lock(controllerMutex); state = currentState; }
    if (action == "configure") {
        const bool deposit = desktopBool(request, "deposit");
        const bool hotbar = desktopBool(request, "hotbar");
        const bool armor = desktopBool(request, "armor");
        const bool totem = desktopBool(request, "totem");
        const bool logging = desktopBool(request, "logging");
        const int delay = bedrock::ShulkerDeposit::clampIntervalMs(desktopInt(request, "intervalMs", 1000));
        configuredDetailedLogging = logging;
        configuredShulkerDeposit = deposit; configuredShulkerDepositHotbar = hotbar;
        configuredShulkerDepositIntervalMs = delay; configuredAutoArmor = armor; configuredAutoTotem = totem;
        if (state) {
            state->configureDeposit(deposit, hotbar, delay);
            state->configureRuntime(logging, false, 24);
            state->configureGameplayFeatures(armor, totem, false, false);
        }
        return DesktopValue::object({{"ok", DesktopValue::boolean(true)}});
    }
    if (action == "snapshot") return state ? snapshotValue(state) : DesktopValue::object({{"running", DesktopValue::boolean(false)}});
    if (action == "events") {
        auto events = DesktopValue::array();
        if (state) {
            std::lock_guard lock(state->mutex);
            while (!state->events.empty()) { events.push(std::move(state->events.front())); state->events.pop_front(); }
        }
        return events;
    }
    if (!state) throw std::runtime_error("Сначала запустите реле");
    auto slot = stringProperty(request, "slot");
    if (slot.empty()) slot = "default";
    if (action == "nbt.copy") return DesktopValue::string(state->copyHeldNbtToFile(slot));
    if (action == "nbt.arm") return DesktopValue::string(state->armNbtCraftFromFile(slot));
    if (action == "nbt.off") return DesktopValue::string(state->stopNbtCraftMode());
    if (action == "nbt.status") return DesktopValue::string(state->nbtCraftModeStatus());
    throw std::runtime_error("Неизвестное действие Windows API");
}
} // namespace

extern "C" __declspec(dllexport) const char* __cdecl cpe_call(const char* request) noexcept {
    // The caller owns the returned allocation and must call cpe_free once.
    try {
        std::lock_guard lock(desktopApiMutex);
        std::string result;
        try {
            if (!request || std::strlen(request) > 64 * 1024) throw std::runtime_error("Invalid request");
            result = jsonString(desktopCall(bedrock::JsRuntimeJson::parse(request)));
        } catch (const std::exception& error) {
            result = jsonString(DesktopValue::object({{"error", DesktopValue::string(safeMessage(error.what()))}}));
        } catch (...) {
            result = "{\"error\":\"Unexpected native error\"}";
        }
        auto* bytes = new char[result.size() + 1];
        std::memcpy(bytes, result.c_str(), result.size() + 1);
        return bytes;
    } catch (...) { return nullptr; }
}
extern "C" __declspec(dllexport) void __cdecl cpe_free(const char* bytes) noexcept { delete[] bytes; }
