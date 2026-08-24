#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace bedrock {

// A single-worker FIFO used for Promise reaction jobs. Tasks posted while a
// task is running are appended to the same queue, so continuations never run
// inline and cannot run concurrently with one another.
class JsMicrotaskQueue final {
public:
    using Task = std::function<void()>;

    JsMicrotaskQueue();
    ~JsMicrotaskQueue();

    JsMicrotaskQueue(const JsMicrotaskQueue&) = delete;
    JsMicrotaskQueue& operator=(const JsMicrotaskQueue&) = delete;
    JsMicrotaskQueue(JsMicrotaskQueue&&) = delete;
    JsMicrotaskQueue& operator=(JsMicrotaskQueue&&) = delete;

    static std::shared_ptr<JsMicrotaskQueue> create();

    void enqueue(Task task);
    void enqueueBatch(std::vector<Task> tasks);
    bool isWorkerThread() const noexcept;

private:
    struct State;

    std::shared_ptr<State> state_;
    std::thread worker_;
};

template <typename T>
class JsPromise;

template <>
class JsPromise<void>;

namespace js_promise_detail {

enum class Status {
    Pending,
    Fulfilled,
    Rejected
};

inline std::exception_ptr normalizeException(std::exception_ptr error) {
    if (error) return error;
    return std::make_exception_ptr(
        std::runtime_error("JsPromise rejected without an exception")
    );
}

template <typename T>
struct State {
    explicit State(std::shared_ptr<JsMicrotaskQueue> queueValue)
        : queue(std::move(queueValue)) {}

    std::shared_ptr<JsMicrotaskQueue> queue;
    mutable std::mutex mutex;
    std::condition_variable settled;
    Status status = Status::Pending;
    std::shared_ptr<const T> value;
    std::exception_ptr error;
    std::vector<JsMicrotaskQueue::Task> continuations;
};

template <>
struct State<void> {
    explicit State(std::shared_ptr<JsMicrotaskQueue> queueValue)
        : queue(std::move(queueValue)) {}

    std::shared_ptr<JsMicrotaskQueue> queue;
    mutable std::mutex mutex;
    std::condition_variable settled;
    Status status = Status::Pending;
    std::exception_ptr error;
    std::vector<JsMicrotaskQueue::Task> continuations;
};

template <typename T>
void publishContinuations(
    const std::shared_ptr<State<T>>& state,
    std::vector<JsMicrotaskQueue::Task> continuations
) {
    if (!continuations.empty()) {
        state->queue->enqueueBatch(std::move(continuations));
    }
}

template <typename T>
bool fulfillShared(
    const std::shared_ptr<State<T>>& state,
    std::shared_ptr<const T> value
) {
    std::vector<JsMicrotaskQueue::Task> continuations;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status != Status::Pending) return false;
        state->value = std::move(value);
        state->status = Status::Fulfilled;
        continuations.swap(state->continuations);
    }
    state->settled.notify_all();
    publishContinuations(state, std::move(continuations));
    return true;
}

inline bool fulfill(const std::shared_ptr<State<void>>& state) {
    std::vector<JsMicrotaskQueue::Task> continuations;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status != Status::Pending) return false;
        state->status = Status::Fulfilled;
        continuations.swap(state->continuations);
    }
    state->settled.notify_all();
    publishContinuations(state, std::move(continuations));
    return true;
}

template <typename T>
bool reject(
    const std::shared_ptr<State<T>>& state,
    std::exception_ptr error
) {
    std::vector<JsMicrotaskQueue::Task> continuations;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status != Status::Pending) return false;
        state->error = normalizeException(std::move(error));
        state->status = Status::Rejected;
        continuations.swap(state->continuations);
    }
    state->settled.notify_all();
    publishContinuations(state, std::move(continuations));
    return true;
}

template <typename T>
void addContinuation(
    const std::shared_ptr<State<T>>& state,
    JsMicrotaskQueue::Task continuation
) {
    bool enqueueNow = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->status == Status::Pending) {
            state->continuations.push_back(std::move(continuation));
        } else {
            enqueueNow = true;
        }
    }
    if (enqueueNow) {
        state->queue->enqueue(std::move(continuation));
    }
}

template <typename T>
struct AsyncResultTraitsImpl {
    using value_type = T;
    static constexpr bool isPromise = false;
    static constexpr bool isFuture = false;
};

template <typename T>
struct AsyncResultTraitsImpl<JsPromise<T>> {
    using value_type = T;
    static constexpr bool isPromise = true;
    static constexpr bool isFuture = false;
};

template <typename T>
struct AsyncResultTraitsImpl<std::future<T>> {
    using value_type = T;
    static constexpr bool isPromise = false;
    static constexpr bool isFuture = true;
};

template <typename T>
using AsyncResultTraits = AsyncResultTraitsImpl<std::decay_t<T>>;

template <typename T>
using UnwrappedAsyncResult = typename AsyncResultTraits<T>::value_type;

} // namespace js_promise_detail

template <typename T>
class JsPromise {
    static_assert(!std::is_void_v<T>);

public:
    using value_type = T;
    using ResolveFunction = std::function<void(T)>;
    using RejectFunction = std::function<void(std::exception_ptr)>;

    JsPromise() = default;
    JsPromise(const JsPromise&) = default;
    JsPromise(JsPromise&&) noexcept = default;
    JsPromise& operator=(const JsPromise&) = default;
    JsPromise& operator=(JsPromise&&) noexcept = default;

    static JsPromise resolved(
        std::shared_ptr<JsMicrotaskQueue> queue,
        T value
    ) {
        auto promise = pending(std::move(queue));
        js_promise_detail::fulfillShared<T>(
            promise.state_,
            std::make_shared<T>(std::move(value))
        );
        return promise;
    }

    // ECMAScript `new Promise(executor)` counterpart. The executor runs
    // synchronously; a throw rejects the new promise, while retained resolver
    // functions may settle it later (or deliberately never settle it).
    template <typename F>
    static JsPromise create(
        std::shared_ptr<JsMicrotaskQueue> queue,
        F&& executor
    ) {
        auto promise = pending(std::move(queue));
        const auto state = promise.state_;
        ResolveFunction resolve = [state](T value) {
            js_promise_detail::fulfillShared<T>(
                state,
                std::make_shared<T>(std::move(value))
            );
        };
        RejectFunction reject = [state](std::exception_ptr error) {
            js_promise_detail::reject(state, std::move(error));
        };
        try {
            std::invoke(
                std::forward<F>(executor),
                std::move(resolve),
                std::move(reject)
            );
        } catch (...) {
            js_promise_detail::reject(
                state,
                std::current_exception()
            );
        }
        return promise;
    }

    static JsPromise rejected(
        std::shared_ptr<JsMicrotaskQueue> queue,
        std::exception_ptr error
    ) {
        auto promise = pending(std::move(queue));
        js_promise_detail::reject(promise.state_, std::move(error));
        return promise;
    }

    static JsPromise rejected(
        std::shared_ptr<JsMicrotaskQueue> queue,
        std::string message
    ) {
        return rejected(
            std::move(queue),
            std::make_exception_ptr(std::runtime_error(std::move(message)))
        );
    }

    static JsPromise fromFuture(
        std::shared_ptr<JsMicrotaskQueue> queue,
        std::future<T> future
    ) {
        auto promise = pending(std::move(queue));
        adoptFuture(promise.state_, std::move(future));
        return promise;
    }

    // Invokes callable synchronously, as evaluation of an expression before a
    // JavaScript await would be. A thrown exception becomes a rejection. A
    // returned JsPromise or std::future is adopted without blocking the caller.
    template <typename F>
    static JsPromise fromSynchronous(
        std::shared_ptr<JsMicrotaskQueue> queue,
        F&& callable
    ) {
        using Callable = std::decay_t<F>;
        using RawResult = std::invoke_result_t<Callable&>;
        using Result = js_promise_detail::UnwrappedAsyncResult<RawResult>;
        static_assert(
            std::is_same_v<Result, T>,
            "JsPromise::fromSynchronous result type does not match T"
        );

        auto promise = pending(std::move(queue));
        try {
            Callable function(std::forward<F>(callable));
            auto result = std::invoke(function);
            settleReturned(promise.state_, std::move(result));
        } catch (...) {
            js_promise_detail::reject(
                promise.state_,
                std::current_exception()
            );
        }
        return promise;
    }

    bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    bool isPending() const {
        auto state = requireState();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->status == js_promise_detail::Status::Pending;
    }

    bool isFulfilled() const {
        auto state = requireState();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->status == js_promise_detail::Status::Fulfilled;
    }

    bool isRejected() const {
        auto state = requireState();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->status == js_promise_detail::Status::Rejected;
    }

    std::shared_ptr<JsMicrotaskQueue> queue() const {
        return requireState()->queue;
    }

    std::shared_ptr<const T> getShared() const {
        auto state = requireState();
        std::unique_lock<std::mutex> lock(state->mutex);
        state->settled.wait(lock, [&state] {
            return state->status != js_promise_detail::Status::Pending;
        });
        const auto status = state->status;
        const auto error = state->error;
        const auto value = state->value;
        lock.unlock();

        if (status == js_promise_detail::Status::Rejected) {
            std::rethrow_exception(error);
        }
        return value;
    }

    T get() const {
        static_assert(
            std::is_copy_constructible_v<T>,
            "JsPromise<T>::get() requires a copy-constructible T; use "
            "getShared() for move-only values"
        );
        return *getShared();
    }

    template <typename F>
    auto then(F&& onFulfilled) const
        -> JsPromise<js_promise_detail::UnwrappedAsyncResult<
            std::invoke_result_t<std::decay_t<F>&, const T&>
        >> {
        using Callback = std::decay_t<F>;
        using RawResult = std::invoke_result_t<Callback&, const T&>;
        using Result = js_promise_detail::UnwrappedAsyncResult<RawResult>;

        auto source = requireState();
        auto output = JsPromise<Result>::pending(source->queue);
        try {
            auto callback = std::make_shared<Callback>(
                std::forward<F>(onFulfilled)
            );
            auto continuation = [source, destination = output.state_, callback] {
                std::shared_ptr<const T> value;
                std::exception_ptr error;
                js_promise_detail::Status status;
                {
                    std::lock_guard<std::mutex> lock(source->mutex);
                    status = source->status;
                    value = source->value;
                    error = source->error;
                }

                if (status == js_promise_detail::Status::Rejected) {
                    js_promise_detail::reject(destination, std::move(error));
                    return;
                }
                if (status != js_promise_detail::Status::Fulfilled) {
                    js_promise_detail::reject(
                        destination,
                        std::make_exception_ptr(std::logic_error(
                            "JsPromise continuation ran before settlement"
                        ))
                    );
                    return;
                }

                try {
                    if constexpr (std::is_void_v<RawResult>) {
                        std::invoke(*callback, *value);
                        js_promise_detail::fulfill(destination);
                    } else {
                        auto result = std::invoke(*callback, *value);
                        JsPromise<Result>::settleReturned(
                            destination,
                            std::move(result)
                        );
                    }
                } catch (...) {
                    js_promise_detail::reject(
                        destination,
                        std::current_exception()
                    );
                }
            };
            js_promise_detail::addContinuation(
                source,
                std::move(continuation)
            );
        } catch (...) {
            js_promise_detail::reject(
                output.state_,
                std::current_exception()
            );
        }
        return output;
    }

    // Promise.prototype.catch analogue. The callback receives the preserved
    // native exception object and may recover with T, JsPromise<T>, or
    // std::future<T>. Like a JS reaction, it is always scheduled on the queue.
    template <typename F>
    JsPromise catchError(F&& onRejected) const {
        using Callback = std::decay_t<F>;
        using RawResult = std::invoke_result_t<
            Callback&,
            std::exception_ptr
        >;
        using Result = js_promise_detail::UnwrappedAsyncResult<RawResult>;
        static_assert(
            std::is_same_v<Result, T>,
            "JsPromise<T>::catchError callback must recover with T, "
            "JsPromise<T>, or std::future<T>"
        );

        auto source = requireState();
        auto output = JsPromise::pending(source->queue);
        try {
            auto callback = std::make_shared<Callback>(
                std::forward<F>(onRejected)
            );
            auto continuation = [
                source,
                destination = output.state_,
                callback
            ] {
                std::shared_ptr<const T> value;
                std::exception_ptr error;
                js_promise_detail::Status status;
                {
                    std::lock_guard<std::mutex> lock(source->mutex);
                    status = source->status;
                    value = source->value;
                    error = source->error;
                }

                if (status == js_promise_detail::Status::Fulfilled) {
                    js_promise_detail::fulfillShared(
                        destination,
                        std::move(value)
                    );
                    return;
                }
                if (status != js_promise_detail::Status::Rejected) {
                    js_promise_detail::reject(
                        destination,
                        std::make_exception_ptr(std::logic_error(
                            "JsPromise rejection continuation ran before "
                            "settlement"
                        ))
                    );
                    return;
                }

                try {
                    auto result = std::invoke(*callback, error);
                    JsPromise::settleReturned(
                        destination,
                        std::move(result)
                    );
                } catch (...) {
                    js_promise_detail::reject(
                        destination,
                        std::current_exception()
                    );
                }
            };
            js_promise_detail::addContinuation(
                source,
                std::move(continuation)
            );
        } catch (...) {
            js_promise_detail::reject(
                output.state_,
                std::current_exception()
            );
        }
        return output;
    }

private:
    using State = js_promise_detail::State<T>;

    explicit JsPromise(std::shared_ptr<State> state)
        : state_(std::move(state)) {}

    static JsPromise pending(std::shared_ptr<JsMicrotaskQueue> queue) {
        if (!queue) {
            throw std::invalid_argument(
                "JsPromise requires a JsMicrotaskQueue"
            );
        }
        return JsPromise(std::make_shared<State>(std::move(queue)));
    }

    std::shared_ptr<State> requireState() const {
        if (!state_) {
            throw std::logic_error("Invalid JsPromise");
        }
        return state_;
    }

    static void adoptFuture(
        const std::shared_ptr<State>& destination,
        std::future<T> future
    ) {
        try {
            std::thread waiter(
                [destination, future = std::move(future)]() mutable {
                    try {
                        auto value = future.get();
                        js_promise_detail::fulfillShared<T>(
                            destination,
                            std::make_shared<T>(std::move(value))
                        );
                    } catch (...) {
                        js_promise_detail::reject(
                            destination,
                            std::current_exception()
                        );
                    }
                }
            );
            waiter.detach();
        } catch (...) {
            js_promise_detail::reject(
                destination,
                std::current_exception()
            );
        }
    }

    static void adoptPromise(
        const std::shared_ptr<State>& destination,
        const JsPromise& sourcePromise
    ) {
        if (!sourcePromise.state_) {
            js_promise_detail::reject(
                destination,
                std::make_exception_ptr(std::logic_error(
                    "Cannot adopt an invalid JsPromise"
                ))
            );
            return;
        }
        if (destination == sourcePromise.state_) {
            js_promise_detail::reject(
                destination,
                std::make_exception_ptr(std::logic_error(
                    "A JsPromise cannot resolve to itself"
                ))
            );
            return;
        }

        auto source = sourcePromise.state_;
        auto continuation = [source, destination] {
            std::shared_ptr<const T> value;
            std::exception_ptr error;
            js_promise_detail::Status status;
            {
                std::lock_guard<std::mutex> lock(source->mutex);
                status = source->status;
                value = source->value;
                error = source->error;
            }
            if (status == js_promise_detail::Status::Fulfilled) {
                js_promise_detail::fulfillShared(destination, std::move(value));
            } else if (status == js_promise_detail::Status::Rejected) {
                js_promise_detail::reject(destination, std::move(error));
            } else {
                js_promise_detail::reject(
                    destination,
                    std::make_exception_ptr(std::logic_error(
                        "JsPromise adoption ran before settlement"
                    ))
                );
            }
        };
        try {
            js_promise_detail::addContinuation(
                source,
                std::move(continuation)
            );
        } catch (...) {
            js_promise_detail::reject(
                destination,
                std::current_exception()
            );
        }
    }

    template <typename R>
    static void settleReturned(
        const std::shared_ptr<State>& destination,
        R&& result
    ) {
        using Traits = js_promise_detail::AsyncResultTraits<R>;
        if constexpr (Traits::isPromise) {
            adoptPromise(destination, result);
        } else if constexpr (Traits::isFuture) {
            adoptFuture(destination, std::forward<R>(result));
        } else {
            js_promise_detail::fulfillShared<T>(
                destination,
                std::make_shared<T>(std::forward<R>(result))
            );
        }
    }

    std::shared_ptr<State> state_;

    template <typename>
    friend class JsPromise;
};

template <>
class JsPromise<void> {
public:
    using value_type = void;
    using ResolveFunction = std::function<void()>;
    using RejectFunction = std::function<void(std::exception_ptr)>;

    JsPromise() = default;
    JsPromise(const JsPromise&) = default;
    JsPromise(JsPromise&&) noexcept = default;
    JsPromise& operator=(const JsPromise&) = default;
    JsPromise& operator=(JsPromise&&) noexcept = default;

    static JsPromise resolved(std::shared_ptr<JsMicrotaskQueue> queue) {
        auto promise = pending(std::move(queue));
        js_promise_detail::fulfill(promise.state_);
        return promise;
    }

    template <typename F>
    static JsPromise create(
        std::shared_ptr<JsMicrotaskQueue> queue,
        F&& executor
    ) {
        auto promise = pending(std::move(queue));
        const auto state = promise.state_;
        ResolveFunction resolve = [state] {
            js_promise_detail::fulfill(state);
        };
        RejectFunction reject = [state](std::exception_ptr error) {
            js_promise_detail::reject(state, std::move(error));
        };
        try {
            std::invoke(
                std::forward<F>(executor),
                std::move(resolve),
                std::move(reject)
            );
        } catch (...) {
            js_promise_detail::reject(
                state,
                std::current_exception()
            );
        }
        return promise;
    }

    static JsPromise rejected(
        std::shared_ptr<JsMicrotaskQueue> queue,
        std::exception_ptr error
    ) {
        auto promise = pending(std::move(queue));
        js_promise_detail::reject(promise.state_, std::move(error));
        return promise;
    }

    static JsPromise rejected(
        std::shared_ptr<JsMicrotaskQueue> queue,
        std::string message
    ) {
        return rejected(
            std::move(queue),
            std::make_exception_ptr(std::runtime_error(std::move(message)))
        );
    }

    static JsPromise fromFuture(
        std::shared_ptr<JsMicrotaskQueue> queue,
        std::future<void> future
    ) {
        auto promise = pending(std::move(queue));
        adoptFuture(promise.state_, std::move(future));
        return promise;
    }

    template <typename F>
    static JsPromise fromSynchronous(
        std::shared_ptr<JsMicrotaskQueue> queue,
        F&& callable
    ) {
        using Callable = std::decay_t<F>;
        using RawResult = std::invoke_result_t<Callable&>;
        using Result = js_promise_detail::UnwrappedAsyncResult<RawResult>;
        static_assert(
            std::is_void_v<Result>,
            "JsPromise<void>::fromSynchronous callable must return void, "
            "JsPromise<void>, or std::future<void>"
        );

        auto promise = pending(std::move(queue));
        try {
            Callable function(std::forward<F>(callable));
            if constexpr (std::is_void_v<RawResult>) {
                std::invoke(function);
                js_promise_detail::fulfill(promise.state_);
            } else {
                auto result = std::invoke(function);
                settleReturned(promise.state_, std::move(result));
            }
        } catch (...) {
            js_promise_detail::reject(
                promise.state_,
                std::current_exception()
            );
        }
        return promise;
    }

    bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

    bool isPending() const {
        auto state = requireState();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->status == js_promise_detail::Status::Pending;
    }

    bool isFulfilled() const {
        auto state = requireState();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->status == js_promise_detail::Status::Fulfilled;
    }

    bool isRejected() const {
        auto state = requireState();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->status == js_promise_detail::Status::Rejected;
    }

    std::shared_ptr<JsMicrotaskQueue> queue() const {
        return requireState()->queue;
    }

    void get() const {
        auto state = requireState();
        std::unique_lock<std::mutex> lock(state->mutex);
        state->settled.wait(lock, [&state] {
            return state->status != js_promise_detail::Status::Pending;
        });
        const auto status = state->status;
        const auto error = state->error;
        lock.unlock();

        if (status == js_promise_detail::Status::Rejected) {
            std::rethrow_exception(error);
        }
    }

    template <typename F>
    auto then(F&& onFulfilled) const
        -> JsPromise<js_promise_detail::UnwrappedAsyncResult<
            std::invoke_result_t<std::decay_t<F>&>
        >> {
        using Callback = std::decay_t<F>;
        using RawResult = std::invoke_result_t<Callback&>;
        using Result = js_promise_detail::UnwrappedAsyncResult<RawResult>;

        auto source = requireState();
        auto output = JsPromise<Result>::pending(source->queue);
        try {
            auto callback = std::make_shared<Callback>(
                std::forward<F>(onFulfilled)
            );
            auto continuation = [source, destination = output.state_, callback] {
                std::exception_ptr error;
                js_promise_detail::Status status;
                {
                    std::lock_guard<std::mutex> lock(source->mutex);
                    status = source->status;
                    error = source->error;
                }

                if (status == js_promise_detail::Status::Rejected) {
                    js_promise_detail::reject(destination, std::move(error));
                    return;
                }
                if (status != js_promise_detail::Status::Fulfilled) {
                    js_promise_detail::reject(
                        destination,
                        std::make_exception_ptr(std::logic_error(
                            "JsPromise continuation ran before settlement"
                        ))
                    );
                    return;
                }

                try {
                    if constexpr (std::is_void_v<RawResult>) {
                        std::invoke(*callback);
                        js_promise_detail::fulfill(destination);
                    } else {
                        auto result = std::invoke(*callback);
                        JsPromise<Result>::settleReturned(
                            destination,
                            std::move(result)
                        );
                    }
                } catch (...) {
                    js_promise_detail::reject(
                        destination,
                        std::current_exception()
                    );
                }
            };
            js_promise_detail::addContinuation(
                source,
                std::move(continuation)
            );
        } catch (...) {
            js_promise_detail::reject(
                output.state_,
                std::current_exception()
            );
        }
        return output;
    }

    template <typename F>
    JsPromise catchError(F&& onRejected) const {
        using Callback = std::decay_t<F>;
        using RawResult = std::invoke_result_t<
            Callback&,
            std::exception_ptr
        >;
        using Result = js_promise_detail::UnwrappedAsyncResult<RawResult>;
        static_assert(
            std::is_void_v<Result>,
            "JsPromise<void>::catchError callback must return void, "
            "JsPromise<void>, or std::future<void>"
        );

        auto source = requireState();
        auto output = JsPromise::pending(source->queue);
        try {
            auto callback = std::make_shared<Callback>(
                std::forward<F>(onRejected)
            );
            auto continuation = [
                source,
                destination = output.state_,
                callback
            ] {
                std::exception_ptr error;
                js_promise_detail::Status status;
                {
                    std::lock_guard<std::mutex> lock(source->mutex);
                    status = source->status;
                    error = source->error;
                }

                if (status == js_promise_detail::Status::Fulfilled) {
                    js_promise_detail::fulfill(destination);
                    return;
                }
                if (status != js_promise_detail::Status::Rejected) {
                    js_promise_detail::reject(
                        destination,
                        std::make_exception_ptr(std::logic_error(
                            "JsPromise rejection continuation ran before "
                            "settlement"
                        ))
                    );
                    return;
                }

                try {
                    if constexpr (std::is_void_v<RawResult>) {
                        std::invoke(*callback, error);
                        js_promise_detail::fulfill(destination);
                    } else {
                        auto result = std::invoke(*callback, error);
                        JsPromise::settleReturned(
                            destination,
                            std::move(result)
                        );
                    }
                } catch (...) {
                    js_promise_detail::reject(
                        destination,
                        std::current_exception()
                    );
                }
            };
            js_promise_detail::addContinuation(
                source,
                std::move(continuation)
            );
        } catch (...) {
            js_promise_detail::reject(
                output.state_,
                std::current_exception()
            );
        }
        return output;
    }

private:
    using State = js_promise_detail::State<void>;

    explicit JsPromise(std::shared_ptr<State> state)
        : state_(std::move(state)) {}

    static JsPromise pending(std::shared_ptr<JsMicrotaskQueue> queue) {
        if (!queue) {
            throw std::invalid_argument(
                "JsPromise requires a JsMicrotaskQueue"
            );
        }
        return JsPromise(std::make_shared<State>(std::move(queue)));
    }

    std::shared_ptr<State> requireState() const {
        if (!state_) {
            throw std::logic_error("Invalid JsPromise");
        }
        return state_;
    }

    static void adoptFuture(
        const std::shared_ptr<State>& destination,
        std::future<void> future
    ) {
        try {
            std::thread waiter(
                [destination, future = std::move(future)]() mutable {
                    try {
                        future.get();
                        js_promise_detail::fulfill(destination);
                    } catch (...) {
                        js_promise_detail::reject(
                            destination,
                            std::current_exception()
                        );
                    }
                }
            );
            waiter.detach();
        } catch (...) {
            js_promise_detail::reject(
                destination,
                std::current_exception()
            );
        }
    }

    static void adoptPromise(
        const std::shared_ptr<State>& destination,
        const JsPromise& sourcePromise
    ) {
        if (!sourcePromise.state_) {
            js_promise_detail::reject(
                destination,
                std::make_exception_ptr(std::logic_error(
                    "Cannot adopt an invalid JsPromise"
                ))
            );
            return;
        }
        if (destination == sourcePromise.state_) {
            js_promise_detail::reject(
                destination,
                std::make_exception_ptr(std::logic_error(
                    "A JsPromise cannot resolve to itself"
                ))
            );
            return;
        }

        auto source = sourcePromise.state_;
        auto continuation = [source, destination] {
            std::exception_ptr error;
            js_promise_detail::Status status;
            {
                std::lock_guard<std::mutex> lock(source->mutex);
                status = source->status;
                error = source->error;
            }
            if (status == js_promise_detail::Status::Fulfilled) {
                js_promise_detail::fulfill(destination);
            } else if (status == js_promise_detail::Status::Rejected) {
                js_promise_detail::reject(destination, std::move(error));
            } else {
                js_promise_detail::reject(
                    destination,
                    std::make_exception_ptr(std::logic_error(
                        "JsPromise adoption ran before settlement"
                    ))
                );
            }
        };
        try {
            js_promise_detail::addContinuation(
                source,
                std::move(continuation)
            );
        } catch (...) {
            js_promise_detail::reject(
                destination,
                std::current_exception()
            );
        }
    }

    template <typename R>
    static void settleReturned(
        const std::shared_ptr<State>& destination,
        R&& result
    ) {
        using Traits = js_promise_detail::AsyncResultTraits<R>;
        if constexpr (Traits::isPromise) {
            adoptPromise(destination, result);
        } else if constexpr (Traits::isFuture) {
            adoptFuture(destination, std::forward<R>(result));
        } else {
            static_assert(
                std::is_void_v<R>,
                "JsPromise<void> cannot resolve from a non-void value"
            );
        }
    }

    std::shared_ptr<State> state_;

    template <typename>
    friend class JsPromise;
};

} // namespace bedrock
