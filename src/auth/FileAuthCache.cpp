#include <bedrock/auth/FileAuthCache.hpp>

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bedrock {
namespace {

JsRuntimeValue emptyObject() {
    return JsRuntimeValue::object();
}

std::vector<std::uint16_t> nodeUtf16CodeUnits(std::string_view utf8) {
    std::vector<std::uint16_t> result;
    result.reserve(utf8.size());

    for (std::size_t offset = 0; offset < utf8.size();) {
        const auto first = static_cast<std::uint8_t>(utf8[offset]);
        std::uint32_t codePoint = 0;
        std::size_t width = 0;

        if (first < 0x80) {
            codePoint = first;
            width = 1;
        } else if (first >= 0xc2 && first <= 0xdf &&
                   offset + 1 < utf8.size()) {
            const auto second = static_cast<std::uint8_t>(utf8[offset + 1]);
            if ((second & 0xc0) == 0x80) {
                codePoint = ((first & 0x1fU) << 6) | (second & 0x3fU);
                width = 2;
            }
        } else if (first >= 0xe0 && first <= 0xef &&
                   offset + 2 < utf8.size()) {
            const auto second = static_cast<std::uint8_t>(utf8[offset + 1]);
            const auto third = static_cast<std::uint8_t>(utf8[offset + 2]);
            const bool continuation =
                (second & 0xc0) == 0x80 && (third & 0xc0) == 0x80;
            const bool scalar = first != 0xe0 || second >= 0xa0;
            if (continuation && scalar) {
                codePoint = ((first & 0x0fU) << 12) |
                    ((second & 0x3fU) << 6) | (third & 0x3fU);
                width = 3;
            }
        } else if (first >= 0xf0 && first <= 0xf4 &&
                   offset + 3 < utf8.size()) {
            const auto second = static_cast<std::uint8_t>(utf8[offset + 1]);
            const auto third = static_cast<std::uint8_t>(utf8[offset + 2]);
            const auto fourth = static_cast<std::uint8_t>(utf8[offset + 3]);
            const bool continuation =
                (second & 0xc0) == 0x80 &&
                (third & 0xc0) == 0x80 &&
                (fourth & 0xc0) == 0x80;
            const bool scalar =
                (first != 0xf0 || second >= 0x90) &&
                (first != 0xf4 || second < 0x90);
            if (continuation && scalar) {
                codePoint = ((first & 0x07U) << 18) |
                    ((second & 0x3fU) << 12) |
                    ((third & 0x3fU) << 6) | (fourth & 0x3fU);
                width = 4;
            }
        }

        if (width == 0) {
            result.push_back(first);
            ++offset;
            continue;
        }

        if (codePoint <= 0xffffU) {
            result.push_back(static_cast<std::uint16_t>(codePoint));
        } else {
            codePoint -= 0x10000U;
            const std::uint16_t high = static_cast<std::uint16_t>(
                0xd800U + (codePoint >> 10)
            );
            const std::uint16_t low = static_cast<std::uint16_t>(
                0xdc00U + (codePoint & 0x3ffU)
            );
            result.push_back(high);
            result.push_back(low);
        }
        offset += width;
    }

    return result;
}

std::vector<std::uint8_t> nodeBinaryBytes(std::string_view utf8) {
    // crypto.update(string, 'binary') truncates each UTF-16 code unit to its
    // low byte, including surrogate halves.
    const auto units = nodeUtf16CodeUnits(utf8);
    std::vector<std::uint8_t> result;
    result.reserve(units.size());
    for (const auto unit : units) {
        result.push_back(static_cast<std::uint8_t>(unit & 0xffU));
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

std::array<unsigned char, EVP_MAX_MD_SIZE> sha1(
    const std::vector<std::uint8_t>& input,
    unsigned int& digestSize
) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    if (EVP_Digest(
            input.data(),
            input.size(),
            digest.data(),
            &digestSize,
            EVP_sha1(),
            nullptr
        ) != 1) {
        throw std::runtime_error("EVP SHA-1 failed for auth cache username");
    }
    return digest;
}

void writeCacheFile(
    const std::filesystem::path& location,
    const JsRuntimeValue& value
) {
    std::ofstream file(location, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error(
            "failed to open auth cache for writing: " + location.string()
        );
    }

    const auto serialized = JsRuntimeJson::stringify(value);
    if (!serialized) {
        // fs.writeFileSync(path, JSON.stringify(undefined)) rejects its data
        // argument after FileCache has already updated this.cache.
        throw std::runtime_error(
            "The data argument must be of type string or an instance of Buffer, "
            "TypedArray, or DataView. Received undefined"
        );
    }
    const std::string& json = *serialized;
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!file) {
        throw std::runtime_error(
            "failed to write auth cache: " + location.string()
        );
    }
}

JsRuntimeValue readCacheFile(const std::filesystem::path& location) {
    std::ifstream file(location, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "failed to open auth cache for reading: " + location.string()
        );
    }
    const std::string json {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };
    if (!file.eof() && file.fail()) {
        throw std::runtime_error(
            "failed to read auth cache: " + location.string()
        );
    }
    return JsRuntimeJson::parse(json);
}

void spreadInto(
    JsRuntimeValue& target,
    const JsRuntimeValue& source
) {
    switch (source.kind()) {
        case JsRuntimeValue::Kind::Object:
        case JsRuntimeValue::Kind::Array:
            for (const auto& property : source.ownProperties()) {
                target.set(property.key, property.value);
            }
            return;
        case JsRuntimeValue::Kind::String: {
            const auto units = nodeUtf16CodeUnits(source.stringValue());
            for (std::size_t i = 0; i < units.size(); ++i) {
                target.set(
                    std::to_string(i),
                    JsRuntimeValue::string(wtf8CodeUnit(units[i]))
                );
            }
            return;
        }
        case JsRuntimeValue::Kind::Undefined:
        case JsRuntimeValue::Kind::Null:
        case JsRuntimeValue::Kind::Bool:
        case JsRuntimeValue::Kind::Number:
        case JsRuntimeValue::Kind::Function:
        case JsRuntimeValue::Kind::Opaque:
            return;
    }
}

JsRuntimeValue shallowSpread(
    const std::optional<JsRuntimeValue>& current,
    const JsRuntimeValue& update
) {
    auto merged = JsRuntimeValue::object();
    if (current.has_value()) spreadInto(merged, *current);
    spreadInto(merged, update);
    return merged;
}

} // namespace

struct FileAuthCache::State {
    explicit State(std::filesystem::path value)
        : location(std::move(value)) {}

    std::filesystem::path location;
    std::optional<JsRuntimeValue> cache;
    std::mutex mutex;
};

FileAuthCache::FileAuthCache(std::filesystem::path cacheLocation)
    : FileAuthCache(std::make_shared<State>(std::move(cacheLocation))) {}

FileAuthCache::FileAuthCache(
    const std::filesystem::path& cacheRoot,
    AuthCacheFactoryOptions options
) : FileAuthCache(cacheLocationFor(
        cacheRoot,
        options.cacheName,
        options.username
    )) {}

FileAuthCache::FileAuthCache(std::shared_ptr<State> state)
    : AuthCache(
          [state]() -> AuthCacheValueFuture {
              try {
                  std::lock_guard<std::mutex> lock(state->mutex);
                  if (!state->cache.has_value()) {
                      try {
                          state->cache = readCacheFile(state->location);
                      } catch (...) {
                          const JsRuntimeValue resetValue = emptyObject();
                          writeCacheFile(state->location, resetValue);
                          state->cache = resetValue;
                      }
                  }
                  return makeReadyAuthCacheFuture(*state->cache);
              } catch (...) {
                  return makeRejectedAuthCacheFuture<AuthCacheValue>(
                      std::current_exception()
                  );
              }
          },
          [state](AuthCacheValue update) -> AuthCacheVoidFuture {
              try {
                  std::lock_guard<std::mutex> lock(state->mutex);
                  JsRuntimeValue merged = shallowSpread(state->cache, update);
                  // FileCache assigns this.cache before writeFileSync(), so a
                  // failed write still leaves the new in-memory value visible.
                  state->cache = std::move(merged);
                  writeCacheFile(state->location, *state->cache);
                  return makeReadyAuthCacheFuture();
              } catch (...) {
                  return makeRejectedAuthCacheFuture<void>(
                      std::current_exception()
                  );
              }
          },
          [state]() -> AuthCacheValueFuture {
              try {
                  std::lock_guard<std::mutex> lock(state->mutex);
                  // FileCache.reset() writes {}, but deliberately does not set
                  // this.cache. Preserve that observable runtime behavior.
                  auto value = emptyObject();
                  writeCacheFile(state->location, value);
                  return makeReadyAuthCacheFuture(std::move(value));
              } catch (...) {
                  return makeRejectedAuthCacheFuture<AuthCacheValue>(
                      std::current_exception()
                  );
              }
          },
          [state](AuthCacheValue value) -> AuthCacheVoidFuture {
              try {
                  std::lock_guard<std::mutex> lock(state->mutex);
                  // Assignment precedes writeFileSync() in FileCache.js.
                  state->cache = std::move(value);
                  writeCacheFile(state->location, *state->cache);
                  return makeReadyAuthCacheFuture();
              } catch (...) {
                  return makeRejectedAuthCacheFuture<void>(
                      std::current_exception()
                  );
              }
          }
      ),
      state_(std::move(state)) {}

const std::filesystem::path& FileAuthCache::cacheLocation() const noexcept {
    return state_->location;
}

std::string FileAuthCache::usernameHash(std::string_view username) {
    const auto input = nodeBinaryBytes(username);
    unsigned int digestSize = 0;
    const auto digest = sha1(input, digestSize);
    if (digestSize < 3) {
        throw std::runtime_error("SHA-1 returned an invalid digest length");
    }

    std::ostringstream result;
    result << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t i = 0; i < 3; ++i) {
        result << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return result.str();
}

std::string FileAuthCache::cacheFileName(
    std::string_view cacheName,
    std::string_view username
) {
    return usernameHash(username) + "_" + std::string(cacheName) +
        "-cache.json";
}

std::filesystem::path FileAuthCache::cacheLocationFor(
    const std::filesystem::path& cacheRoot,
    std::string_view cacheName,
    std::string_view username
) {
    return cacheRoot / cacheFileName(cacheName, username);
}

AuthCachePtr makeFileAuthCache(
    const std::filesystem::path& cacheRoot,
    AuthCacheFactoryOptions options
) {
    return std::make_shared<FileAuthCache>(cacheRoot, std::move(options));
}

} // namespace bedrock
