#include <bedrock/auth/MsalHttpClient.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>

#if !defined(_WIN32) || defined(__CYGWIN__)
#include <sys/wait.h>
#endif

namespace bedrock {
namespace {

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char byte) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
    });
    return value;
}

bool asciiCaseInsensitiveEqual(
    std::string_view left,
    std::string_view right
) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto leftByte = static_cast<unsigned char>(left[index]);
        const auto rightByte = static_cast<unsigned char>(right[index]);
        if (std::tolower(leftByte) != std::tolower(rightByte)) return false;
    }
    return true;
}

std::string trimHeaderValue(std::string value) {
    while (!value.empty() &&
        (value.back() == '\r' || value.back() == '\n' ||
         value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t begin = 0;
    while (begin < value.size() &&
        (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    return value.substr(begin);
}

std::string shellQuotePath(std::string_view value) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__MSYS__)
    // These are generated filesystem paths, not user-controlled arguments.
    // Quotes cannot occur in a valid Windows path.
    if (value.find('"') != std::string_view::npos) {
        throw std::runtime_error("invalid quote in temporary path");
    }
    std::string result = "\"";
    result.append(value);
    result.push_back('"');
    return result;
#else
    std::string result = "'";
    for (const char byte : value) {
        if (byte == '\'') result += "'\\''";
        else result.push_back(byte);
    }
    result.push_back('\'');
    return result;
#endif
}

std::string curlConfigQuote(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char byte : value) {
        switch (byte) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\t': result += "\\t"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\v': result += "\\v"; break;
            default: result.push_back(byte); break;
        }
    }
    result.push_back('"');
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

void writeFile(const std::filesystem::path& path, std::string_view value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "failed to create temporary MSAL HTTP request file"
        );
    }
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) {
        throw std::runtime_error(
            "failed to write temporary MSAL HTTP request file"
        );
    }
}

std::string latin1ToUtf8(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        if (byte < 0x80) {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back(static_cast<char>(0xc0 | (byte >> 6)));
            result.push_back(static_cast<char>(0x80 | (byte & 0x3f)));
        }
    }
    return result;
}

void appendUtf8Replacement(std::string& output) {
    output += "\xef\xbf\xbd";
}

// Buffer.toString() uses the UTF-8 decoder with replacement, while reading a
// file into std::string preserves invalid bytes. Convert to the same logical
// JavaScript string before JSON.parse and before exposing bodyText.
std::string nodeUtf8Decode(std::string_view bytes) {
    std::string output;
    output.reserve(bytes.size());
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[index]);
        if (first < 0x80) {
            output.push_back(static_cast<char>(first));
            ++index;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t minimum = 0;
        std::uint32_t codePoint = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            minimum = 0x80;
            codePoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            minimum = 0x800;
            codePoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            minimum = 0x10000;
            codePoint = first & 0x07;
        } else {
            appendUtf8Replacement(output);
            ++index;
            continue;
        }

        bool continuationValid = true;
        std::size_t continuationOffset = 1;
        for (; continuationOffset < length &&
             index + continuationOffset < bytes.size();
             ++continuationOffset) {
            const auto continuation =
                static_cast<unsigned char>(bytes[index + continuationOffset]);
            if ((continuation & 0xc0) != 0x80) {
                continuationValid = false;
                break;
            }
            codePoint = (codePoint << 6) | (continuation & 0x3f);
        }
        if (!continuationValid) {
            appendUtf8Replacement(output);
            // WHATWG's decoder has already consumed the valid continuation
            // prefix but reprocesses the first non-continuation byte.
            index += continuationOffset;
            continue;
        }
        if (continuationOffset < length) {
            // A truncated sequence whose available suffix is entirely made
            // of continuation bytes becomes one U+FFFD in Buffer.toString().
            appendUtf8Replacement(output);
            index = bytes.size();
            continue;
        }
        if (codePoint < minimum ||
            codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            appendUtf8Replacement(output);
            ++index;
            continue;
        }
        output.append(bytes.substr(index, length));
        index += length;
    }
    return output;
}

class TemporaryRequestDirectory {
public:
    TemporaryRequestDirectory() {
        const auto root = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 128; ++attempt) {
            const auto nonce =
                (static_cast<std::uint64_t>(random()) << 32) ^ random() ^
                static_cast<std::uint64_t>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch().count()
                );
            path_ = root /
                ("bedrock-msal-http-" + std::to_string(nonce));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) return;
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error(
                    "failed to create temporary MSAL HTTP directory: " +
                    error.message()
                );
            }
        }
        throw std::runtime_error(
            "failed to allocate a unique temporary MSAL HTTP directory"
        );
    }

    ~TemporaryRequestDirectory() {
        if (path_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct ParsedHeaderBlock {
    std::string statusMessage;
    std::vector<std::pair<std::string, std::string>> rawHeaders;
};

std::vector<std::string_view> splitHeaderBlocks(std::string_view raw) {
    std::vector<std::string_view> blocks;
    std::size_t start = 0;
    while (start < raw.size()) {
        const auto crlfEnd = raw.find("\r\n\r\n", start);
        const auto lfEnd = raw.find("\n\n", start);
        std::size_t end = std::string_view::npos;
        std::size_t separatorSize = 0;
        if (crlfEnd != std::string_view::npos &&
            (lfEnd == std::string_view::npos || crlfEnd <= lfEnd)) {
            end = crlfEnd;
            separatorSize = 4;
        } else if (lfEnd != std::string_view::npos) {
            end = lfEnd;
            separatorSize = 2;
        }
        if (end == std::string_view::npos) {
            blocks.emplace_back(raw.substr(start));
            break;
        }
        blocks.emplace_back(raw.substr(start, end - start));
        start = end + separatorSize;
    }
    return blocks;
}

ParsedHeaderBlock parseFinalHeaderBlock(std::string_view raw) {
    // curl -D can contain proxy/intermediate blocks. Retain only the final
    // HTTP response block, matching Node's final IncomingMessage.
    std::string_view block;
    const auto blocks = splitHeaderBlocks(raw);
    for (auto iterator = blocks.rbegin(); iterator != blocks.rend();
         ++iterator) {
        if (iterator->starts_with("HTTP/")) {
            block = *iterator;
            break;
        }
    }
    ParsedHeaderBlock result;
    if (block.empty()) return result;

    std::istringstream lines{std::string(block)};
    std::string line;
    std::getline(lines, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto firstSpace = line.find(' ');
    if (firstSpace != std::string::npos) {
        const auto secondSpace = line.find(' ', firstSpace + 1);
        if (secondSpace != std::string::npos) {
            auto messageStart = secondSpace + 1;
            while (messageStart < line.size() &&
                line[messageStart] == ' ') {
                ++messageStart;
            }
            result.statusMessage = latin1ToUtf8(
                std::string_view(line).substr(messageStart)
            );
        }
    }

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            if (!result.rawHeaders.empty()) {
                result.rawHeaders.back().second += " ";
                result.rawHeaders.back().second += trimHeaderValue(
                    latin1ToUtf8(line)
                );
            }
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        result.rawHeaders.emplace_back(
            asciiLower(line.substr(0, colon)),
            trimHeaderValue(latin1ToUtf8(line.substr(colon + 1)))
        );
    }
    return result;
}

bool nodeDiscardsDuplicate(std::string_view name) noexcept {
    // Node's IncomingMessage.headers default (joinDuplicateHeaders=false).
    constexpr std::string_view names[] = {
        "age",
        "authorization",
        "content-length",
        "content-type",
        "etag",
        "expires",
        "from",
        "host",
        "if-modified-since",
        "if-unmodified-since",
        "last-modified",
        "location",
        "max-forwards",
        "proxy-authorization",
        "referer",
        "retry-after",
        "server",
        "user-agent"
    };
    return std::find(std::begin(names), std::end(names), name) !=
        std::end(names);
}

struct NormalizedHeaders {
    std::vector<std::pair<std::string, std::string>> scalar;
    JsRuntimeValue object = JsRuntimeValue::object();
};

NormalizedHeaders normalizeResponseHeaders(
    const std::vector<std::pair<std::string, std::string>>& rawHeaders
) {
    struct HeaderValue {
        std::string name;
        std::vector<std::string> values;
    };
    std::vector<HeaderValue> values;
    std::unordered_map<std::string, std::size_t> indices;
    for (const auto& [rawName, value] : rawHeaders) {
        const auto name = asciiLower(rawName);
        const auto found = indices.find(name);
        if (found == indices.end()) {
            indices.emplace(name, values.size());
            values.push_back(HeaderValue{name, {value}});
        } else if (!nodeDiscardsDuplicate(name)) {
            values[found->second].values.push_back(value);
        }
    }

    NormalizedHeaders result;
    result.scalar.reserve(values.size());
    for (const auto& item : values) {
        if (item.name == "set-cookie") {
            std::vector<JsRuntimeValue> cookies;
            cookies.reserve(item.values.size());
            std::string flattened;
            for (std::size_t index = 0; index < item.values.size(); ++index) {
                if (index != 0) flattened += ", ";
                flattened += item.values[index];
                cookies.push_back(JsRuntimeValue::string(item.values[index]));
            }
            result.object.set(
                item.name,
                JsRuntimeValue::array(std::move(cookies))
            );
            result.scalar.emplace_back(item.name, std::move(flattened));
            continue;
        }

        const std::string_view separator =
            item.name == "cookie" ? "; " : ", ";
        std::string joined;
        for (std::size_t index = 0; index < item.values.size(); ++index) {
            if (index != 0) joined += separator;
            joined += item.values[index];
        }
        result.object.set(item.name, JsRuntimeValue::string(joined));
        result.scalar.emplace_back(item.name, std::move(joined));
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> normalizeRequestHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers,
    bool isPost,
    std::size_t contentLength
) {
    std::vector<std::pair<std::string, std::string>> result;
    std::unordered_map<std::string, std::size_t> indices;
    for (const auto& [name, value] : headers) {
        const auto lowerName = asciiLower(name);
        if (isPost && lowerName == "content-length") continue;
        const auto found = indices.find(lowerName);
        if (found == indices.end()) {
            indices.emplace(lowerName, result.size());
            result.emplace_back(name, value);
        } else {
            // A JS headers object may contain differently-cased spellings.
            // Node retains the first position but the last spelling/value.
            result[found->second] = {name, value};
        }
    }
    if (isPost) {
        result.emplace_back("Content-Length", std::to_string(contentLength));
    }
    return result;
}

bool hasHeader(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view name
) noexcept {
    return std::any_of(headers.begin(), headers.end(), [name](const auto& item) {
        return asciiCaseInsensitiveEqual(item.first, name);
    });
}

bool validHeaderName(std::string_view name) noexcept {
    if (name.empty()) return false;
    constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
    for (const unsigned char byte : name) {
        if (!std::isalnum(byte) &&
            punctuation.find(static_cast<char>(byte)) ==
                std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool validHeaderValue(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const char rawByte) {
        const auto byte = static_cast<unsigned char>(rawByte);
        return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
    });
}

std::size_t utf16LengthOfUtf8(std::string_view value) noexcept {
    // MSAL request bodies are form-urlencoded ASCII. Counting well-formed
    // UTF-8 too preserves JavaScript String.length for injected test bodies.
    std::size_t length = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first < 0x80) {
            ++length;
            ++index;
        } else if (first >= 0xf0 && first <= 0xf4 &&
            index + 3 < value.size() &&
            (static_cast<unsigned char>(value[index + 1]) & 0xc0) == 0x80 &&
            (static_cast<unsigned char>(value[index + 2]) & 0xc0) == 0x80 &&
            (static_cast<unsigned char>(value[index + 3]) & 0xc0) == 0x80) {
            length += 2;
            index += 4;
        } else if (first >= 0xe0 && first <= 0xef &&
            index + 2 < value.size() &&
            (static_cast<unsigned char>(value[index + 1]) & 0xc0) == 0x80 &&
            (static_cast<unsigned char>(value[index + 2]) & 0xc0) == 0x80) {
            ++length;
            index += 3;
        } else if (first >= 0xc2 && first <= 0xdf &&
            index + 1 < value.size() &&
            (static_cast<unsigned char>(value[index + 1]) & 0xc0) == 0x80) {
            ++length;
            index += 2;
        } else {
            ++length;
            ++index;
        }
    }
    return length;
}

std::string parseBodyErrorDescription(
    int status,
    std::string_view statusMessage,
    const JsRuntimeValue& headersObject
) {
    std::string errorDescriptionHelper;
    if (status >= 400 && status <= 499) {
        errorDescriptionHelper = "A client";
    } else if (status >= 500 && status <= 599) {
        errorDescriptionHelper = "A server";
    } else {
        errorDescriptionHelper = "An unknown";
    }
    const auto headersJson =
        JsRuntimeJson::stringify(headersObject).value_or("{}");
    return errorDescriptionHelper +
        " error occured.\nHttp status code: " + std::to_string(status) +
        "\nHttp status message: " +
        std::string(statusMessage.empty() ? "Unknown" : statusMessage) +
        "\nHeaders: " + headersJson;
}

JsRuntimeValue parseResponseBody(
    std::string_view body,
    int status,
    std::string_view statusMessage,
    const JsRuntimeValue& headersObject
) {
    try {
        return JsRuntimeJson::parse(body);
    } catch (...) {
        auto parsed = JsRuntimeValue::object();
        std::string errorType;
        if (status >= 400 && status <= 499) {
            errorType = "client_error";
        } else if (status >= 500 && status <= 599) {
            errorType = "server_error";
        } else {
            errorType = "unknown_error";
        }
        parsed.set("error", JsRuntimeValue::string(std::move(errorType)));
        parsed.set(
            "error_description",
            JsRuntimeValue::string(parseBodyErrorDescription(
                status,
                statusMessage,
                headersObject
            ))
        );
        return parsed;
    }
}

int decodedSystemExitCode(int status) noexcept {
#if !defined(_WIN32) || defined(__CYGWIN__)
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
#endif
    return status;
}

struct ParsedHttpUrl {
    std::string host;
    std::string port;
};

std::optional<ParsedHttpUrl> parseHttpUrl(std::string_view url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) return std::nullopt;
    const auto scheme = asciiLower(std::string(url.substr(0, schemeEnd)));
    if (scheme != "http" && scheme != "https") return std::nullopt;
    const auto authorityStart = schemeEnd + 3;
    const auto authorityEnd = url.find_first_of("/?#", authorityStart);
    auto authority = url.substr(
        authorityStart,
        authorityEnd == std::string_view::npos
            ? url.size() - authorityStart
            : authorityEnd - authorityStart
    );
    const auto at = authority.rfind('@');
    if (at != std::string_view::npos) authority.remove_prefix(at + 1);
    if (authority.empty()) return std::nullopt;
    for (const unsigned char byte : authority) {
        if (byte <= 0x20 || byte == 0x7f) return std::nullopt;
    }

    ParsedHttpUrl result;
    if (authority.front() == '[') {
        const auto bracket = authority.find(']');
        if (bracket == std::string_view::npos || bracket == 1) {
            return std::nullopt;
        }
        result.host = std::string(authority.substr(1, bracket - 1));
        if (bracket + 1 < authority.size()) {
            if (authority[bracket + 1] != ':') return std::nullopt;
            result.port = std::string(authority.substr(bracket + 2));
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) return std::nullopt;
            result.host = std::string(authority.substr(0, colon));
            result.port = std::string(authority.substr(colon + 1));
        } else {
            result.host = std::string(authority);
        }
    }
    if (result.host.empty()) return std::nullopt;
    result.host = asciiLower(std::move(result.host));
    if (result.port.empty()) result.port = scheme == "http" ? "80" : "443";
    if (!std::all_of(result.port.begin(), result.port.end(), [](char byte) {
            return byte >= '0' && byte <= '9';
        })) {
        return std::nullopt;
    }
    char* portEnd = nullptr;
    const auto port = std::strtol(result.port.c_str(), &portEnd, 10);
    if (!portEnd || *portEnd != '\0' || port < 0 || port > 65535) {
        return std::nullopt;
    }
    result.port = std::to_string(port);
    return result;
}

std::string curlFailureMessage(
    int exitCode,
    std::string detail,
    const ParsedHttpUrl& url
) {
    if (exitCode == 6) {
        return "Error: getaddrinfo ENOTFOUND " + url.host;
    }
    if (exitCode == 7) {
        return "Error: connect ECONNREFUSED " + url.host + ":" + url.port;
    }
    detail = trimHeaderValue(std::move(detail));
    const std::string prefix =
        "curl: (" + std::to_string(exitCode) + ") ";
    if (detail.starts_with(prefix)) detail.erase(0, prefix.size());
    if (detail.empty()) detail = "HTTP transport failed";
    // HttpClient wraps the emitted request Error in another Error, so its
    // rejection message starts with `Error: `.
    return "Error: " + detail;
}

MsalHttpResponse sendBlocking(MsalHttpRequest request) {
    const auto parsedUrl = parseHttpUrl(request.url);
    if (!parsedUrl) throw std::invalid_argument("Invalid URL");
    const auto lowerMethod = asciiLower(
        request.method.empty() ? std::string("post") : request.method
    );
    const bool isPost = lowerMethod == "post";
    const bool isGet = lowerMethod == "get";
    if (!isPost && !isGet) {
        throw std::invalid_argument("Unsupported MSAL HTTP method");
    }
    if (request.maxRedirects < 0) {
        throw std::invalid_argument("maxRedirects must not be negative");
    }

    TemporaryRequestDirectory temporary;
    const auto requestBody = temporary.path() / "request.bin";
    const auto responseBody = temporary.path() / "response.bin";
    const auto responseHeaders = temporary.path() / "headers.txt";
    const auto statusFile = temporary.path() / "status.txt";
    const auto errorFile = temporary.path() / "stderr.txt";
    const auto configFile = temporary.path() / "curl.conf";
    writeFile(requestBody, request.body);

    auto headers = normalizeRequestHeaders(
        request.headers,
        isPost,
        utf16LengthOfUtf8(request.body)
    );
    for (const auto& [name, value] : headers) {
        if (!validHeaderName(name)) {
            throw std::invalid_argument("Header name must be a valid HTTP token");
        }
        if (!validHeaderValue(value)) {
            throw std::invalid_argument(
                "Invalid character in HTTP header content"
            );
        }
    }

    // curl otherwise adds User-Agent, Accept, and (for --data-binary)
    // Content-Type fields that Node's http.request does not add. Node's
    // default non-keepalive agent does add Connection: close.
    if (!hasHeader(headers, "User-Agent")) {
        headers.emplace_back("User-Agent", std::string());
    }
    if (!hasHeader(headers, "Accept")) {
        headers.emplace_back("Accept", std::string());
    }
    if (!hasHeader(headers, "Content-Type")) {
        headers.emplace_back("Content-Type", std::string());
    }
    if (!hasHeader(headers, "Expect")) {
        headers.emplace_back("Expect", std::string());
    }
    if (!hasHeader(headers, "Connection")) {
        headers.emplace_back("Connection", "close");
    }

    std::string config;
    config += "silent\nshow-error\nhttp1.1\ngloboff\n";
    if (request.decompress) config += "compressed\n";
    if (request.maxRedirects > 0) {
        config += "location\nmax-redirs = " +
            std::to_string(request.maxRedirects) + "\n";
    }
    // HttpClient(proxyUrl === "") does not honor ambient proxy variables.
    config += "noproxy = \"*\"\n";
    // An explicit POST would force curl to retain POST across 301/302. Axios'
    // follow-redirects adapter switches that login redirect to GET, so let
    // data-binary select POST when redirect following is enabled.
    if (!(isPost && request.maxRedirects > 0)) {
        config += "request = " +
            curlConfigQuote(isPost ? "POST" : "GET") + "\n";
    }
    config += "dump-header = " + curlConfigQuote(responseHeaders.string()) +
        "\n";
    config += "output = " + curlConfigQuote(responseBody.string()) + "\n";
    config += "write-out = \"%{http_code}\\n%{url_effective}\"\n";
    for (const auto& [name, value] : headers) {
        // `Header:` removes curl's internal header; `Header;` asks curl to
        // transmit a deliberately empty header supplied by the caller.
        const bool callerSupplied = std::any_of(
            request.headers.begin(),
            request.headers.end(),
            [&name](const auto& item) {
                return asciiCaseInsensitiveEqual(item.first, name);
            }
        );
        const auto line = value.empty()
            ? name + (callerSupplied ? ";" : ":")
            : name + ": " + value;
        config += "header = " + curlConfigQuote(line) + "\n";
    }
    if (isPost) {
        config += "data-binary = " +
            curlConfigQuote("@" + requestBody.string()) + "\n";
    }
    config += "url = " + curlConfigQuote(request.url) + "\n";
    writeFile(configFile, config);

    // --disable must be the first curl option so a user .curlrc cannot alter
    // the transport. All user-controlled values live in curl.conf, avoiding
    // cmd.exe/MSYS/POSIX shell interpolation.
    const std::string command = "curl --disable --config " +
        shellQuotePath(configFile.string()) + " >" +
        shellQuotePath(statusFile.string()) + " 2>" +
        shellQuotePath(errorFile.string());

    const int exitCode = decodedSystemExitCode(std::system(command.c_str()));
    if (exitCode != 0) {
        throw std::runtime_error(curlFailureMessage(
            exitCode,
            readFile(errorFile),
            *parsedUrl
        ));
    }

    const auto transportResult = readFile(statusFile);
    const auto resultSeparator = transportResult.find('\n');
    const auto statusText = trimHeaderValue(transportResult.substr(
        0,
        resultSeparator
    ));
    const auto effectiveUrl = resultSeparator == std::string::npos
        ? std::string()
        : trimHeaderValue(transportResult.substr(resultSeparator + 1));
    char* statusEnd = nullptr;
    const long status = std::strtol(statusText.c_str(), &statusEnd, 10);
    if (!statusEnd || statusEnd != statusText.c_str() + statusText.size() ||
        status < 100 || status > 999) {
        throw std::runtime_error(
            "MSAL HTTP transport returned an invalid status code"
        );
    }

    MsalHttpResponse response;
    response.status = static_cast<int>(status);
    response.url = effectiveUrl;
    const auto parsedHeaderBlock =
        parseFinalHeaderBlock(readFile(responseHeaders));
    auto normalizedHeaders =
        normalizeResponseHeaders(parsedHeaderBlock.rawHeaders);
    response.headers = std::move(normalizedHeaders.scalar);
    response.headersObject = std::move(normalizedHeaders.object);
    response.bodyText = nodeUtf8Decode(readFile(responseBody));
    response.body = parseResponseBody(
        response.bodyText,
        response.status,
        parsedHeaderBlock.statusMessage,
        response.headersObject
    );
    return response;
}

} // namespace

const std::string* MsalHttpRequest::header(
    std::string_view name
) const noexcept {
    for (auto iterator = headers.rbegin(); iterator != headers.rend();
         ++iterator) {
        if (asciiCaseInsensitiveEqual(iterator->first, name)) {
            return &iterator->second;
        }
    }
    return nullptr;
}

const std::string* MsalHttpResponse::header(
    std::string_view lowerCaseName
) const noexcept {
    for (const auto& [name, value] : headers) {
        if (name == lowerCaseName) return &value;
    }
    return nullptr;
}

CurlMsalHttpClient::CurlMsalHttpClient(
    std::shared_ptr<JsMicrotaskQueue> microtaskQueue
) : microtaskQueue_(std::move(microtaskQueue)) {
    if (!microtaskQueue_) {
        throw std::invalid_argument(
            "CurlMsalHttpClient requires a JsMicrotaskQueue"
        );
    }
}

JsPromise<MsalHttpResponse> CurlMsalHttpClient::send(
    MsalHttpRequest request
) {
    return JsPromise<MsalHttpResponse>::fromFuture(
        microtaskQueue_,
        std::async(
            std::launch::async,
            [request = std::move(request)]() mutable {
                return sendBlocking(std::move(request));
            }
        )
    );
}

} // namespace bedrock
