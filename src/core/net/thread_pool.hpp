#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace bisondb::net {

// Fixed-size worker pool. Tasks are move-only callables (std::function
// requires copyability and std::move_only_function is C++23, so a minimal
// type-erased box is used). Exceptions thrown by tasks are swallowed —
// a failing task never kills its worker. stop() drains the queue: queued
// tasks still run before the workers join.
class ThreadPool {
  public:
    explicit ThreadPool(std::size_t threads = std::thread::hardware_concurrency()) {
        if (threads == 0) {
            threads = 2;
        }
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() { stop(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F> void submit(F&& fn) {
        auto task = std::make_unique<TaskImpl<std::decay_t<F>>>(std::forward<F>(fn));
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return; // tasks submitted after stop() are dropped
            }
            queue_.push(std::move(task));
        }
        cv_.notify_one();
    }

    // Drain-and-join: lets queued tasks finish, then joins all workers.
    // Idempotent.
    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        cv_.notify_all();
        for (std::thread& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    std::size_t threadCount() const noexcept { return workers_.size(); }

  private:
    struct TaskBase {
        virtual ~TaskBase() = default;
        virtual void run() = 0;
    };
    template <typename F> struct TaskImpl : TaskBase {
        explicit TaskImpl(F f) : fn(std::move(f)) {}
        void run() override { fn(); }
        F fn;
    };

    void workerLoop() {
        while (true) {
            std::unique_ptr<TaskBase> task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    return; // stopping and drained
                }
                task = std::move(queue_.front());
                queue_.pop();
            }
            try {
                task->run();
            } catch (...) {
                // Task exceptions must not kill the worker.
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<TaskBase>> queue_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace bisondb::net
