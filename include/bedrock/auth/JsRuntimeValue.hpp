#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrock {

class JsRuntimeValue;
class JsRuntimeObject;
class JsRuntimeArray;
class JsRuntimeDate;
class JsRuntimeMap;
struct JsRuntimeProperty;

namespace js_runtime_detail {

class NativeFunctionStorageBase {
public:
    virtual ~NativeFunctionStorageBase() = default;
    virtual std::type_index signatureType() const noexcept = 0;
};

template <typename Signature>
class NativeFunctionStorage final : public NativeFunctionStorageBase {
public:
    explicit NativeFunctionStorage(std::function<Signature> value)
        : function(std::move(value)) {}

    std::type_index signatureType() const noexcept override {
        return typeid(Signature);
    }

    std::function<Signature> function;
};

template <typename Signature>
struct NativeFunctionArity;

template <typename Result, typename... Arguments>
struct NativeFunctionArity<Result(Arguments...)> {
    static constexpr std::size_t value = sizeof...(Arguments);
};

class OpaqueStorageBase {
public:
    virtual ~OpaqueStorageBase() = default;
    virtual std::type_index valueType() const noexcept = 0;
    virtual std::shared_ptr<const void> identity() const noexcept = 0;
};

template <typename T>
class OpaqueStorage final : public OpaqueStorageBase {
public:
    explicit OpaqueStorage(std::shared_ptr<T> value)
        : value(std::move(value)) {}

    std::type_index valueType() const noexcept override {
        return typeid(T);
    }

    std::shared_ptr<const void> identity() const noexcept override {
        return std::shared_ptr<const void>(
            value,
            static_cast<const void*>(value.get())
        );
    }

    std::shared_ptr<T> value;
};

} // namespace js_runtime_detail

// Auth-only JavaScript value graph. Object and array copies retain their node
// identity, allowing supplied MSAL configuration graphs to be passed through
// and mutated with JavaScript reference semantics.
class JsRuntimeValue {
public:
    enum class Kind {
        Undefined,
        Null,
        Bool,
        Number,
        String,
        Object,
        Array,
        Date,
        Map,
        Function,
        Opaque
    };

    using ObjectPtr = std::shared_ptr<JsRuntimeObject>;
    using ArrayPtr = std::shared_ptr<JsRuntimeArray>;
    using DatePtr = std::shared_ptr<JsRuntimeDate>;
    using MapPtr = std::shared_ptr<JsRuntimeMap>;

    JsRuntimeValue() noexcept = default;

    static JsRuntimeValue undefined() noexcept;
    static JsRuntimeValue null() noexcept;
    static JsRuntimeValue boolean(bool value) noexcept;
    static JsRuntimeValue number(double value) noexcept;
    static JsRuntimeValue string(std::string value);
    static JsRuntimeValue string(std::string_view value);
    static JsRuntimeValue string(const char* value);

    static JsRuntimeValue object();
    static JsRuntimeValue object(
        std::initializer_list<JsRuntimeProperty> properties
    );
    static JsRuntimeValue array();
    static JsRuntimeValue array(
        std::initializer_list<JsRuntimeValue> elements
    );
    static JsRuntimeValue array(std::vector<JsRuntimeValue> elements);
    static JsRuntimeValue date(double millisecondsSinceEpoch);
    static JsRuntimeValue map();

    template <typename Signature, typename Callable>
    static JsRuntimeValue function(Callable&& callable);

    template <typename Signature, typename Callable>
    static JsRuntimeValue namedFunction(
        std::string name,
        Callable&& callable
    );

    template <typename T>
    static JsRuntimeValue opaque(std::shared_ptr<T> value);

    Kind kind() const noexcept { return kind_; }

    bool isUndefined() const noexcept { return kind_ == Kind::Undefined; }
    bool isNull() const noexcept { return kind_ == Kind::Null; }
    bool isBool() const noexcept { return kind_ == Kind::Bool; }
    bool isNumber() const noexcept { return kind_ == Kind::Number; }
    bool isString() const noexcept { return kind_ == Kind::String; }
    bool isObject() const noexcept { return kind_ == Kind::Object; }
    bool isArray() const noexcept { return kind_ == Kind::Array; }
    bool isDate() const noexcept { return kind_ == Kind::Date; }
    bool isMap() const noexcept { return kind_ == Kind::Map; }
    bool isFunction() const noexcept { return kind_ == Kind::Function; }
    bool isOpaque() const noexcept { return kind_ == Kind::Opaque; }

    bool boolValue() const;
    double numberValue() const;
    const std::string& stringValue() const;
    std::string& stringValue();
    const ObjectPtr& objectNode() const;
    ObjectPtr& objectNode();
    const ArrayPtr& arrayNode() const;
    ArrayPtr& arrayNode();
    const DatePtr& dateNode() const;
    DatePtr& dateNode();
    const MapPtr& mapNode() const;
    MapPtr& mapNode();

    double dateMilliseconds() const;
    bool dateIsValid() const;
    std::string dateIsoString() const;

    // Object-style convenience API. Arrays accept canonical integer-index
    // keys here as well; their non-index enumerable properties are retained.
    const JsRuntimeValue* get(std::string_view key) const;
    JsRuntimeValue* get(std::string_view key);
    bool hasOwn(std::string_view key) const;
    void set(std::string key, JsRuntimeValue value);
    std::vector<JsRuntimeProperty> ownProperties() const;

    const JsRuntimeValue* get(std::size_t index) const;
    JsRuntimeValue* get(std::size_t index);
    void set(std::size_t index, JsRuntimeValue value);
    void push(JsRuntimeValue value);
    std::size_t length() const;

    JsRuntimeValue& mapSet(JsRuntimeValue key, JsRuntimeValue value);
    const JsRuntimeValue* mapGet(const JsRuntimeValue& key) const;
    JsRuntimeValue* mapGet(const JsRuntimeValue& key);
    bool mapErase(const JsRuntimeValue& key);
    std::size_t mapSize() const;

    template <typename Signature>
    bool isFunctionOf() const noexcept;

    template <typename Signature>
    std::function<Signature>* functionIf() noexcept;

    template <typename Signature>
    const std::function<Signature>* functionIf() const noexcept;

    template <typename Signature>
    std::function<Signature>& requireFunction();

    template <typename Signature>
    const std::function<Signature>& requireFunction() const;

    template <typename Signature, typename... Args>
    decltype(auto) call(Args&&... args);

    template <typename Signature, typename... Args>
    decltype(auto) call(Args&&... args) const;

    template <typename T>
    std::shared_ptr<T> opaqueIf() const noexcept;

    std::type_index functionSignature() const noexcept;
    std::type_index opaqueType() const noexcept;

    bool truthy() const noexcept;

    // Reference kinds compare their actual shared identity. Primitive kinds
    // follow JavaScript strict-equality rules (including NaN and signed zero).
    bool strictlyEquals(const JsRuntimeValue& other) const noexcept;
    bool sharesIdentityWith(const JsRuntimeValue& other) const noexcept;

private:
    Kind kind_ = Kind::Undefined;
    bool boolValue_ = false;
    double numberValue_ = 0.0;
    std::string stringValue_;
    ObjectPtr objectValue_;
    ArrayPtr arrayValue_;
    DatePtr dateValue_;
    MapPtr mapValue_;
    std::shared_ptr<js_runtime_detail::NativeFunctionStorageBase>
        functionValue_;
    ObjectPtr functionProperties_;
    std::shared_ptr<js_runtime_detail::OpaqueStorageBase> opaqueValue_;
};

struct JsRuntimeProperty {
    JsRuntimeProperty(std::string key, JsRuntimeValue value)
        : key(std::move(key)), value(std::move(value)) {}

    std::string key;
    JsRuntimeValue value;
};

class JsRuntimeObject {
public:
    const JsRuntimeValue* get(std::string_view key) const;
    JsRuntimeValue* get(std::string_view key);
    bool hasOwn(std::string_view key) const;

    // Replacing an existing property never changes its insertion position.
    void set(std::string key, JsRuntimeValue value);
    bool erase(std::string_view key);

    std::size_t size() const noexcept { return properties_.size(); }
    bool empty() const noexcept { return properties_.empty(); }

    // Same enumerable ordering used by Object.keys and JSON.stringify:
    // canonical array indices first in ascending order, then other strings in
    // insertion order.
    std::vector<JsRuntimeProperty> ownProperties() const;

    // Raw insertion order is exposed for graph/introspection code. JSON and
    // public ownProperties() deliberately use ECMAScript key ordering.
    const std::vector<JsRuntimeProperty>& insertionProperties() const noexcept {
        return properties_;
    }

private:
    std::vector<JsRuntimeProperty> properties_;
    std::unordered_map<std::string, std::size_t> indices_;
};

class JsRuntimeArray {
public:
    const JsRuntimeValue* get(std::size_t index) const;
    JsRuntimeValue* get(std::size_t index);
    void set(std::size_t index, JsRuntimeValue value);
    bool erase(std::size_t index);
    void push(JsRuntimeValue value);

    const JsRuntimeValue* get(std::string_view key) const;
    JsRuntimeValue* get(std::string_view key);
    bool hasOwn(std::string_view key) const;
    void set(std::string key, JsRuntimeValue value);
    bool erase(std::string_view key);

    std::size_t length() const noexcept { return elements_.size(); }
    std::vector<JsRuntimeProperty> ownProperties() const;

    const std::vector<std::optional<JsRuntimeValue>>& elements() const noexcept {
        return elements_;
    }

private:
    std::vector<std::optional<JsRuntimeValue>> elements_;
    JsRuntimeObject namedProperties_;
    JsRuntimeValue lengthValue_ = JsRuntimeValue::number(0.0);
};

class JsRuntimeDate {
public:
    explicit JsRuntimeDate(double millisecondsSinceEpoch) noexcept;

    double milliseconds() const noexcept { return milliseconds_; }
    bool valid() const noexcept;
    std::string toISOString() const;

private:
    double milliseconds_;
};

struct JsRuntimeMapEntry {
    JsRuntimeMapEntry(JsRuntimeValue key, JsRuntimeValue value)
        : key(std::move(key)), value(std::move(value)) {}

    JsRuntimeValue key;
    JsRuntimeValue value;
};

class JsRuntimeMap {
public:
    JsRuntimeValue& set(JsRuntimeValue key, JsRuntimeValue value);
    const JsRuntimeValue* get(const JsRuntimeValue& key) const;
    JsRuntimeValue* get(const JsRuntimeValue& key);
    bool erase(const JsRuntimeValue& key);
    void clear();

    std::size_t size() const noexcept { return entries_.size(); }
    const std::vector<JsRuntimeMapEntry>& entries() const noexcept {
        return entries_;
    }

    const JsRuntimeValue* getProperty(std::string_view key) const;
    JsRuntimeValue* getProperty(std::string_view key);
    bool hasOwnProperty(std::string_view key) const;
    void setProperty(std::string key, JsRuntimeValue value);
    std::vector<JsRuntimeProperty> ownProperties() const;

private:
    std::vector<JsRuntimeMapEntry> entries_;
    JsRuntimeObject properties_;
    JsRuntimeValue sizeValue_ = JsRuntimeValue::number(0.0);

    void updateSize() noexcept;
};

class JsRuntimeJson {
public:
    static JsRuntimeValue parse(std::string_view json);

    // Like JSON.stringify, the three non-serializable top-level kinds return
    // JavaScript undefined, represented here as std::nullopt.
    static std::optional<std::string> stringify(
        const JsRuntimeValue& value
    );
};

template <typename Signature, typename Callable>
JsRuntimeValue JsRuntimeValue::function(Callable&& callable) {
    return namedFunction<Signature>(
        std::string(),
        std::forward<Callable>(callable)
    );
}

template <typename Signature, typename Callable>
JsRuntimeValue JsRuntimeValue::namedFunction(
    std::string name,
    Callable&& callable
) {
    JsRuntimeValue result;
    result.kind_ = Kind::Function;
    result.functionValue_ = std::make_shared<
        js_runtime_detail::NativeFunctionStorage<Signature>
    >(std::function<Signature>(std::forward<Callable>(callable)));
    result.functionProperties_ = std::make_shared<JsRuntimeObject>();
    // Arrow functions expose these two non-enumerable own properties in this
    // order. JsRuntimeValue::ownProperties deliberately remains the analogue
    // of Object.keys and therefore omits them.
    result.functionProperties_->set(
        "length",
        JsRuntimeValue::number(static_cast<double>(
            js_runtime_detail::NativeFunctionArity<Signature>::value
        ))
    );
    result.functionProperties_->set(
        "name",
        JsRuntimeValue::string(std::move(name))
    );
    return result;
}

template <typename T>
JsRuntimeValue JsRuntimeValue::opaque(std::shared_ptr<T> value) {
    JsRuntimeValue result;
    result.kind_ = Kind::Opaque;
    result.opaqueValue_ = std::make_shared<
        js_runtime_detail::OpaqueStorage<T>
    >(std::move(value));
    return result;
}

template <typename Signature>
bool JsRuntimeValue::isFunctionOf() const noexcept {
    return functionIf<Signature>() != nullptr;
}

template <typename Signature>
std::function<Signature>* JsRuntimeValue::functionIf() noexcept {
    if (kind_ != Kind::Function || !functionValue_) return nullptr;
    const auto typed = std::dynamic_pointer_cast<
        js_runtime_detail::NativeFunctionStorage<Signature>
    >(functionValue_);
    return typed ? &typed->function : nullptr;
}

template <typename Signature>
const std::function<Signature>* JsRuntimeValue::functionIf() const noexcept {
    if (kind_ != Kind::Function || !functionValue_) return nullptr;
    const auto typed = std::dynamic_pointer_cast<const
        js_runtime_detail::NativeFunctionStorage<Signature>
    >(functionValue_);
    return typed ? &typed->function : nullptr;
}

template <typename Signature>
std::function<Signature>& JsRuntimeValue::requireFunction() {
    auto* value = functionIf<Signature>();
    if (!value) {
        throw std::bad_cast();
    }
    return *value;
}

template <typename Signature>
const std::function<Signature>& JsRuntimeValue::requireFunction() const {
    const auto* value = functionIf<Signature>();
    if (!value) {
        throw std::bad_cast();
    }
    return *value;
}

template <typename Signature, typename... Args>
decltype(auto) JsRuntimeValue::call(Args&&... args) {
    return requireFunction<Signature>()(std::forward<Args>(args)...);
}

template <typename Signature, typename... Args>
decltype(auto) JsRuntimeValue::call(Args&&... args) const {
    return requireFunction<Signature>()(std::forward<Args>(args)...);
}

template <typename T>
std::shared_ptr<T> JsRuntimeValue::opaqueIf() const noexcept {
    if (kind_ != Kind::Opaque || !opaqueValue_) return {};
    const auto typed = std::dynamic_pointer_cast<
        js_runtime_detail::OpaqueStorage<T>
    >(opaqueValue_);
    return typed ? typed->value : std::shared_ptr<T> {};
}

} // namespace bedrock
