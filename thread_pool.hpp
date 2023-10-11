#pragma once

#include "task.hpp"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <mutex>
#include <ranges>
#include <thread>
#include <vector>

namespace coro
{

template <class T, class V>
concept range_of = std::ranges::range<T> && std::same_as<std::ranges::range_value_t<T>, V>;

/**
 * Creates a thread pool that executes arbitrary coroutine tasks in a FIFO scheduler policy.
 * The thread pool by default will create an execution thread per available core on the system.
 *
 * When shutting down, either by the thread pool destructing or by manually calling shutdown()
 * the thread pool will stop accepting new tasks but will complete all tasks that were scheduled
 * prior to the shutdown request.
 */
class thread_pool
{
public:
    /**
     * An operation is an awaitable type with a coroutine to resume the task scheduled on one of
     * the executor threads.
     */
    class operation
    {
        friend class thread_pool;
        /**
         * Only thread_pools can create operations when a task is being scheduled.
         * @param tp The thread pool that created this operation.
         */
        explicit operation(thread_pool& tp) noexcept: m_thread_pool(tp) {}

    public:
        /**
         * Operations always pause so the executing thread can be switched.
         */
        auto await_ready() noexcept -> bool { return false; }

        /**
         * Suspending always returns to the caller (using void return of await_suspend()) and
         * stores the coroutine internally for the executing thread to resume from.
         */
        auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> void {
            m_thread_pool.schedule_impl(awaiting_coroutine);
        }

        /**
         * no-op as this is the function called first by the thread pool's executing thread.
         */
        auto await_resume() noexcept -> void {}

    private:
        /// The thread pool that this operation will execute on.
        thread_pool& m_thread_pool;
    };

    struct options
    {
        /// The number of executor threads for this thread pool.  Uses the hardware concurrency
        /// value by default.
        uint32_t thread_count = std::thread::hardware_concurrency();
    };

    /**
     * @param opts Thread pool configuration options.
     */
    explicit thread_pool(
        options opts = options{
            .thread_count            = std::thread::hardware_concurrency()})
    : m_opts(std::move(opts)) {
        m_threads.reserve(m_opts.thread_count);
        for (uint32_t i = 0; i < m_opts.thread_count; ++i) {
            m_threads.emplace_back([this] { executor(); });
        }
    }

    thread_pool(const thread_pool&)                    = delete;
    thread_pool(thread_pool&&)                         = delete;
    auto operator=(const thread_pool&) -> thread_pool& = delete;
    auto operator=(thread_pool&&) -> thread_pool&      = delete;

    ~thread_pool() {
        shutdown();
    }

    /**
     * @return The number of executor threads for processing tasks.
     */
    auto thread_count() const noexcept -> uint32_t { return m_threads.size(); }

    /**
     * Schedules the currently executing coroutine to be run on this thread pool.  This must be
     * called from within the coroutines function body to schedule the coroutine on the thread pool.
     * @throw std::runtime_error If the thread pool is `shutdown()` scheduling new tasks is not permitted.
     * @return The operation to switch from the calling scheduling thread to the executor thread
     *         pool thread.
     */
    [[nodiscard]] auto schedule() -> operation {
        if (!m_shutdown_requested.load(std::memory_order::relaxed)) [[likely]] {
            m_size.fetch_add(1, std::memory_order::release);
            return operation{*this};
        }

        throw std::runtime_error("coro::thread_pool is shutting down, unable to schedule new tasks.");
    }

    [[nodiscard]] auto schedule_wait(task<>& task) {
        struct task_awaiter {
            auto await_ready() const noexcept -> bool { return false; }
            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> void {
                m_task.promise().set_continuation(awaiting_coroutine);
                m_thread_pool.schedule_impl(m_task.handle());
            }
            auto await_resume() noexcept -> void {}

            coro::task<>& m_task;
            coro::thread_pool& m_thread_pool;
        };

        if (!m_shutdown_requested.load(std::memory_order::relaxed)) [[likely]] {
            m_size.fetch_add(1, std::memory_order::release);
            return task_awaiter{task, *this};
        }

        throw std::runtime_error("coro::thread_pool is shutting down, unable to schedule new tasks.");
    }

    /**
     * @throw std::runtime_error If the thread pool is `shutdown()` scheduling new tasks is not permitted.
     * @param f The function to execute on the thread pool.
     * @param args The arguments to call the functor with.
     * @return A task that wraps the given functor to be executed on the thread pool.
     */
    template<typename functor, typename... arguments>
    [[nodiscard]] auto schedule(functor&& f, arguments... args) -> task<decltype(f(std::forward<arguments>(args)...))>
    {
        co_await schedule();

        if constexpr (std::is_same_v<void, decltype(f(std::forward<arguments>(args)...))>)
        {
            f(std::forward<arguments>(args)...);
            co_return;
        }
        else
        {
            co_return f(std::forward<arguments>(args)...);
        }
    }

    /**
     * Schedules any coroutine handle that is ready to be resumed.
     * @param handle The coroutine handle to schedule.
     */
    auto resume(std::coroutine_handle<> handle) noexcept -> void {
        if (handle != nullptr) [[likely]] {
            m_size.fetch_add(1, std::memory_order::release);
            schedule_impl(handle);
        }
    }

    /**
     * Schedules the set of coroutine handles that are ready to be resumed.
     * @param handles The coroutine handles to schedule.
     */
    template<range_of<std::coroutine_handle<>> range_type>
    auto resume(const range_type& handles) noexcept -> void
    {
        m_size.fetch_add(std::size(handles), std::memory_order::release);

        size_t null_handles{0};

        {
            std::scoped_lock lk{m_wait_mutex};
            for (const auto& handle : handles)
            {
                if (handle != nullptr) [[likely]]
                {
                    m_queue.emplace_back(handle);
                }
                else
                {
                    ++null_handles;
                }
            }
        }

        if (null_handles > 0)
        {
            m_size.fetch_sub(null_handles, std::memory_order::release);
        }

        m_wait_cv.notify_one();
    }

    /**
     * Immediately yields the current task and places it at the end of the queue of tasks waiting
     * to be processed.  This will immediately be picked up again once it naturally goes through the
     * FIFO task queue.  This function is useful to yielding long processing tasks to let other tasks
     * get processing time.
     */
    [[nodiscard]] auto yield() -> operation { return schedule(); }

    /**
     * Shutsdown the thread pool.  This will finish any tasks scheduled prior to calling this
     * function but will prevent the thread pool from scheduling any new tasks.  This call is
     * blocking and will wait until all inflight tasks are completed before returnin.
     */
    auto shutdown() noexcept -> void {
        if (m_shutdown_requested.exchange(true, std::memory_order::release) == false) {
            m_wait_cv.notify_all();
            for (auto& thread: m_threads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
        }
    }

    /**
     * @return The number of tasks waiting in the task queue + the executing tasks.
     */
    auto size() const noexcept -> std::size_t { return m_size.load(std::memory_order::acquire); }

    /**
     * @return True if the task queue is empty and zero tasks are currently executing.
     */
    auto empty() const noexcept -> bool { return size() == 0; }

    /**
     * @return The number of tasks waiting in the task queue to be executed.
     */
    auto queue_size() const noexcept -> std::size_t
    {
        // Might not be totally perfect but good enough, avoids acquiring the lock for now.
        std::atomic_thread_fence(std::memory_order::acquire);
        return m_queue.size();
    }

    /**
     * @return True if the task queue is currently empty.
     */
    auto queue_empty() const noexcept -> bool { return queue_size() == 0; }

private:
    /// The configuration options.
    options m_opts;
    /// The background executor threads.
    std::vector<std::thread> m_threads;

    /// Mutex for executor threads to sleep on the condition variable.
    std::mutex m_wait_mutex;
    /// Condition variable for each executor thread to wait on when no tasks are available.
    std::condition_variable m_wait_cv;
    /// FIFO queue of tasks waiting to be executed.
    std::deque<std::coroutine_handle<>> m_queue;
    /**
     * Each background thread runs from this function.
     * @param stop_token Token which signals when shutdown() has been called.
     * @param idx The executor's idx for internal data structure accesses.
     */
    auto executor() -> void {
        auto shutdown_requested = m_shutdown_requested.load(std::memory_order::acquire);
        while (true) {
            std::unique_lock lk{m_wait_mutex};
            m_wait_cv.wait(lk, [this, &shutdown_requested] {
                return !m_queue.empty() ||
                    (shutdown_requested = m_shutdown_requested.load(std::memory_order::acquire));
            });
            if (m_queue.empty()) {
                break;
            }
            auto handle = m_queue.front();
            m_queue.pop_front();
            lk.unlock();
            handle.resume();
            m_size.fetch_sub(1, std::memory_order::release);
        }
    }

    /**
     * @param handle Schedules the given coroutine to be executed upon the first available thread.
     */
    auto schedule_impl(std::coroutine_handle<> handle) noexcept -> void {
        if (handle == nullptr) [[unlikely]] {
            return;
        }

        {
            std::lock_guard lk{m_wait_mutex};
            m_queue.emplace_back(handle);
        }
        m_wait_cv.notify_one();
    }

    /// The number of tasks in the queue + currently executing.
    std::atomic<std::size_t> m_size{0};
    /// Has the thread pool been requested to shut down?
    std::atomic<bool> m_shutdown_requested{false};
};

} // namespace coro