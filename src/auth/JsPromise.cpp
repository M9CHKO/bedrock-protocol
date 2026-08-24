#include <bedrock/auth/JsPromise.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace bedrock {

struct JsMicrotaskQueue::State {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Task> tasks;
    bool stopping = false;
};

JsMicrotaskQueue::JsMicrotaskQueue()
    : state_(std::make_shared<State>()) {
    const auto state = state_;
    worker_ = std::thread([state] {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->ready.wait(lock, [&state] {
                    return state->stopping || !state->tasks.empty();
                });
                if (state->stopping && state->tasks.empty()) return;
                task = std::move(state->tasks.front());
                state->tasks.pop_front();
            }

            // Promise reactions convert their own exceptions to rejections.
            // Keep this final guard so an arbitrary directly-enqueued task
            // cannot terminate the executor thread.
            try {
                task();
            } catch (...) {
            }
        }
    });
}

JsMicrotaskQueue::~JsMicrotaskQueue() {
    const auto state = state_;
    if (state) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stopping = true;
        }
        state->ready.notify_all();
    }

    if (!worker_.joinable()) return;
    if (worker_.get_id() == std::this_thread::get_id()) {
        // The worker captures State rather than this, so it can safely finish
        // draining the queue after the owning object dies on its own thread.
        worker_.detach();
    } else {
        worker_.join();
    }
}

std::shared_ptr<JsMicrotaskQueue> JsMicrotaskQueue::create() {
    return std::make_shared<JsMicrotaskQueue>();
}

void JsMicrotaskQueue::enqueue(Task task) {
    if (!task) {
        throw std::invalid_argument(
            "Cannot enqueue an empty JsMicrotaskQueue task"
        );
    }
    std::vector<Task> tasks;
    tasks.push_back(std::move(task));
    enqueueBatch(std::move(tasks));
}

void JsMicrotaskQueue::enqueueBatch(std::vector<Task> tasks) {
    if (tasks.empty()) return;
    for (const auto& task : tasks) {
        if (!task) {
            throw std::invalid_argument(
                "Cannot enqueue an empty JsMicrotaskQueue task"
            );
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping) {
            throw std::runtime_error("JsMicrotaskQueue is stopping");
        }
        for (auto& task : tasks) {
            state_->tasks.push_back(std::move(task));
        }
    }
    state_->ready.notify_one();
}

bool JsMicrotaskQueue::isWorkerThread() const noexcept {
    return worker_.joinable() &&
        worker_.get_id() == std::this_thread::get_id();
}

} // namespace bedrock
