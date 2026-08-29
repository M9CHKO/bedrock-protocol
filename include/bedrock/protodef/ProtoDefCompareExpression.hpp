#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bedrock::detail {

struct ProtoDefCompareAtom {
    std::string switchKey;
    bool truthy = false;
};

inline std::string_view trimCompareExpression(std::string_view value) {
    while (!value.empty() &&
        std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
        std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

inline bool hasProtoDefLogicalOr(std::string_view expression) {
    return expression.find("||") != std::string_view::npos;
}

// ProtoDef's JavaScript compiler emits compareTo directly as the switch
// expression. Preserve scalar switch keys and evaluate minecraft-data's
// logical-OR form with the same truthy semantics in both encoder and decoder.
template<typename Resolver>
std::optional<std::string> evaluateProtoDefCompareExpression(
    std::string_view expression,
    Resolver&& resolver
) {
    expression = trimCompareExpression(expression);
    if (expression.empty()) return std::nullopt;

    if (!hasProtoDefLogicalOr(expression)) {
        const auto resolved = resolver(expression);
        if (!resolved.has_value()) return std::nullopt;
        return resolved->switchKey;
    }

    std::size_t start = 0;
    while (start <= expression.size()) {
        const auto separator = expression.find("||", start);
        const auto operand = trimCompareExpression(expression.substr(
            start,
            separator == std::string_view::npos
                ? std::string_view::npos
                : separator - start
        ));
        if (operand.empty()) {
            throw std::runtime_error(
                "empty operand in ProtoDef compare expression: " +
                std::string(expression)
            );
        }

        const auto resolved = resolver(operand);
        if (resolved.has_value() && resolved->truthy) {
            return std::string("true");
        }
        if (separator == std::string_view::npos) break;
        start = separator + 2;
    }
    return std::string("false");
}

} // namespace bedrock::detail
