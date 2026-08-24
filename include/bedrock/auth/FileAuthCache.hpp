#pragma once

#include <bedrock/auth/AuthCache.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace bedrock {

// Native counterpart of prismarine-auth/src/common/cache/FileCache.js.
//
// The filename helpers mirror MicrosoftAuthFlow's built-in CacheFactory.  The
// cache itself intentionally keeps FileCache's observable lazy-loading rules:
// setCachedPartial() merges with the in-memory value only, and reset() writes
// an empty object without replacing an already-loaded in-memory value.
class FileAuthCache final : public AuthCache {
public:
    explicit FileAuthCache(std::filesystem::path cacheLocation);

    FileAuthCache(
        const std::filesystem::path& cacheRoot,
        AuthCacheFactoryOptions options
    );

    const std::filesystem::path& cacheLocation() const noexcept;

    // Util.createHash(input): SHA-1 over Node's `binary` (`latin1`) encoding,
    // truncated to the first six lowercase hexadecimal characters.
    static std::string usernameHash(std::string_view username = {});

    static std::string cacheFileName(
        std::string_view cacheName,
        std::string_view username = {}
    );

    static std::filesystem::path cacheLocationFor(
        const std::filesystem::path& cacheRoot,
        std::string_view cacheName,
        std::string_view username = {}
    );

private:
    struct State;

    explicit FileAuthCache(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

AuthCachePtr makeFileAuthCache(
    const std::filesystem::path& cacheRoot,
    AuthCacheFactoryOptions options
);

} // namespace bedrock
