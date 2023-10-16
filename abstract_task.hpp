#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <span>
#include <vector>
#include <fmt/core.h>
#include <coro/task.hpp>

struct abstract_task_node {
    virtual auto task_name() -> std::string = 0;
    virtual auto task() -> coro::task<>& = 0;
    virtual auto predecessors() -> std::span<std::shared_ptr<abstract_task_node>> = 0;
    auto schedule() -> coro::task<>;
    virtual ~abstract_task_node() = default;

private:
    enum class node_state {
        init,
        scheduled,
        running,
        completed,
    };

    std::atomic<node_state> m_state{node_state::init};
    std::atomic<void*> m_awaiting_list{nullptr};

    struct schedule_awaitable {
        auto await_ready() const noexcept -> bool { return false; }
        auto await_suspend(std::coroutine_handle<> awaiting_coroutine) -> bool {
            // check whether the task is already scheduled
            auto expected = node_state::init;
            bool success = self_.m_state.compare_exchange_strong(expected, node_state::scheduled, std::memory_order_relaxed);
            schedule_task task;
            if (success) {
                // if the task was not scheduled, schedule it
                // to schedule the task we need to resume the coroutine
                // inside the coroutine, the task may or may not schedule itself to other executor
                task = self_.on_schedule();
                task.start();
            }
            // check whether the task is already completed
            // the address of task node in m_awaiting_list represents for the completion of the task
            const void* completed = std::addressof(self_);
            m_awaiting_coroutine = awaiting_coroutine;

            // try to atomicly push the awaiter to the awaiting list
            void* old_value = self_.m_awaiting_list.load(std::memory_order_relaxed);
            do {
                if (old_value == completed) {
                    // the task is already completed
                    // resume the awaiting coroutine
                    // returning false means resume the suspended coroutine immediately
                    return false;
                }
                // not completed, the old value is a pointer to an awaiter
                // locally push this to head of the list
                m_next = static_cast<schedule_awaitable*>(old_value);
                
                // try to publish the local list until success
            } while (!self_.m_awaiting_list.compare_exchange_weak(old_value, this, std::memory_order_release, std::memory_order_acquire));

            // successfully add this to waiting list, remain suspended
            return true;
        }
        auto await_resume() noexcept -> void {}

        abstract_task_node& self_;
        std::coroutine_handle<> m_awaiting_coroutine{nullptr};
        schedule_awaitable* m_next{nullptr};
    };

    auto try_schedule_self() -> schedule_awaitable {
        return schedule_awaitable{*this};
    }

    struct schedule_task {
        struct task_promise_type {
            using coroutine_handle = std::coroutine_handle<task_promise_type>;

            task_promise_type(abstract_task_node& task): self_(task) {}

            auto initial_suspend() noexcept  { return std::suspend_always{}; }
            schedule_task get_return_object();

            struct final_awaitable {
                auto await_ready() const noexcept { return false; }
                auto await_suspend(coroutine_handle awaiting_coroutine) noexcept {
                    auto& self = awaiting_coroutine.promise().self_;
                    void* old_value = self.m_awaiting_list.exchange(&self, std::memory_order_acq_rel);
                    auto waiters = static_cast<schedule_awaitable*>(old_value);
                    while (waiters != nullptr) {
                        auto next = waiters->m_next;
                        waiters->m_awaiting_coroutine.resume();
                        waiters = next;
                    }
                }
                auto await_resume() const noexcept {};
            };

            auto final_suspend() noexcept { return final_awaitable{}; }
            auto unhandled_exception() noexcept {
                m_exception_ptr = std::current_exception();
            }

            auto return_void() {
                if (m_exception_ptr) {
                    std::rethrow_exception(m_exception_ptr);
                }
            }

            abstract_task_node& self_;
            std::exception_ptr m_exception_ptr;
        };
        using promise_type = task_promise_type;
        using coroutine_handle = promise_type::coroutine_handle;

        schedule_task(): m_coroutine(nullptr) {}
        schedule_task(coroutine_handle handle): m_coroutine(handle) {}
        schedule_task(const schedule_task&) = delete;
        schedule_task(schedule_task&& other) noexcept : m_coroutine(std::exchange(other.m_coroutine, nullptr)) {}
        schedule_task& operator=(const schedule_task& rhs) = delete;
        schedule_task& operator=(schedule_task&& rhs) noexcept {
            if (this != std::addressof(rhs)) {
                if (m_coroutine != nullptr) {
                    m_coroutine.destroy();
                }

                m_coroutine = std::exchange(rhs.m_coroutine, nullptr);
            }
            return *this;
        }
        ~schedule_task() {
            if (m_coroutine != nullptr) {
                m_coroutine.destroy();
            }
        }

        auto start() -> void {
            if (m_coroutine != nullptr) {
                m_coroutine.resume();
            }
        }

        coroutine_handle m_coroutine;
    };

    auto on_schedule() -> schedule_task {
        co_await task();
    }
};

abstract_task_node::schedule_task abstract_task_node::schedule_task::task_promise_type::get_return_object() {
    return std::coroutine_handle<task_promise_type>::from_promise(*this);
}

namespace coro {

namespace detail {

struct void_value {};

struct when_all_latch {
public:
    when_all_latch(std::size_t count): m_count(count + 1) {}
    when_all_latch(const when_all_latch&) = delete;
    when_all_latch(when_all_latch&&) = delete;
    auto operator=(const when_all_latch&) -> when_all_latch& = delete;
    auto operator=(when_all_latch&&) -> when_all_latch& = delete;

    auto is_ready() const noexcept -> bool { return m_awaiting_coroutine != nullptr && m_awaiting_coroutine.done(); }
    auto try_await(std::coroutine_handle<> awaiting_coroutine) -> bool {
        m_awaiting_coroutine = awaiting_coroutine;
        return m_count.fetch_sub(1, std::memory_order_acquire) > 1;
    }
    auto notify_awaitable_completed() noexcept -> void {
        if (m_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (m_awaiting_coroutine != nullptr) {
                m_awaiting_coroutine.resume();
            }
        }
    }

private:
    std::atomic<std::size_t> m_count;
    std::coroutine_handle<> m_awaiting_coroutine{nullptr};
};

template<typename task_container_type>
class when_all_ready_awaitable;

template<typename return_type>
class when_all_task;

template<typename task_container_type>
class when_all_ready_awaitable
{
public:
    explicit when_all_ready_awaitable(task_container_type&& tasks) noexcept
        : m_latch(std::size(tasks)),
          m_tasks(std::forward<task_container_type>(tasks))
    {
    }

    when_all_ready_awaitable(const when_all_ready_awaitable&) = delete;
    when_all_ready_awaitable(when_all_ready_awaitable&& other) noexcept(
        std::is_nothrow_move_constructible_v<task_container_type>)
        : m_latch(std::move(other.m_latch)),
          m_tasks(std::move(m_tasks))
    {
    }

    auto operator=(const when_all_ready_awaitable&) -> when_all_ready_awaitable& = delete;
    auto operator=(when_all_ready_awaitable&) -> when_all_ready_awaitable&       = delete;

    auto operator co_await() & noexcept
    {
        struct awaiter
        {
            awaiter(when_all_ready_awaitable& awaitable) : m_awaitable(awaitable) {}

            auto await_ready() const noexcept -> bool { return m_awaitable.is_ready(); }

            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
            {
                return m_awaitable.try_await(awaiting_coroutine);
            }

            auto await_resume() noexcept -> task_container_type& { return m_awaitable.m_tasks; }

        private:
            when_all_ready_awaitable& m_awaitable;
        };

        return awaiter{*this};
    }

    auto operator co_await() && noexcept
    {
        struct awaiter
        {
            awaiter(when_all_ready_awaitable& awaitable) : m_awaitable(awaitable) {}

            auto await_ready() const noexcept -> bool { return m_awaitable.is_ready(); }

            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
            {
                return m_awaitable.try_await(awaiting_coroutine);
            }

            auto await_resume() noexcept -> task_container_type&& { return std::move(m_awaitable.m_tasks); }

        private:
            when_all_ready_awaitable& m_awaitable;
        };

        return awaiter{*this};
    }

private:
    auto is_ready() const noexcept -> bool { return m_latch.is_ready(); }

    auto try_await(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
    {
        for (auto& task : m_tasks)
        {
            task.start(m_latch);
        }

        return m_latch.try_await(awaiting_coroutine);
    }

    when_all_latch      m_latch;
    task_container_type m_tasks;
};

template<typename return_type>
class when_all_task_promise
{
public:
    using coroutine_handle_type = std::coroutine_handle<when_all_task_promise<return_type>>;

    when_all_task_promise() noexcept {}

    auto get_return_object() noexcept { return coroutine_handle_type::from_promise(*this); }

    auto initial_suspend() noexcept -> std::suspend_always { return {}; }

    auto final_suspend() noexcept
    {
        struct completion_notifier
        {
            auto await_ready() const noexcept -> bool { return false; }
            auto await_suspend(coroutine_handle_type coroutine) const noexcept -> void
            {
                coroutine.promise().m_latch->notify_awaitable_completed();
            }
            auto await_resume() const noexcept {}
        };

        return completion_notifier{};
    }

    auto unhandled_exception() noexcept { m_exception_ptr = std::current_exception(); }

    auto yield_value(return_type&& value) noexcept
    {
        m_return_value = std::addressof(value);
        return final_suspend();
    }

    auto start(when_all_latch& latch) noexcept -> void
    {
        m_latch = &latch;
        coroutine_handle_type::from_promise(*this).resume();
    }

    auto return_value() & -> return_type&
    {
        if (m_exception_ptr)
        {
            std::rethrow_exception(m_exception_ptr);
        }
        return *m_return_value;
    }

    auto return_value() && -> return_type&&
    {
        if (m_exception_ptr)
        {
            std::rethrow_exception(m_exception_ptr);
        }
        return std::forward(*m_return_value);
    }

private:
    when_all_latch*                 m_latch{nullptr};
    std::exception_ptr              m_exception_ptr;
    std::add_pointer_t<return_type> m_return_value;
};

template<>
class when_all_task_promise<void>
{
public:
    using coroutine_handle_type = std::coroutine_handle<when_all_task_promise<void>>;

    when_all_task_promise() noexcept {}

    auto get_return_object() noexcept { return coroutine_handle_type::from_promise(*this); }

    auto initial_suspend() noexcept -> std::suspend_always { return {}; }

    auto final_suspend() noexcept
    {
        struct completion_notifier
        {
            auto await_ready() const noexcept -> bool { return false; }
            auto await_suspend(coroutine_handle_type coroutine) const noexcept -> void
            {
                coroutine.promise().m_latch->notify_awaitable_completed();
            }
            auto await_resume() const noexcept -> void {}
        };

        return completion_notifier{};
    }

    auto unhandled_exception() noexcept -> void { m_exception_ptr = std::current_exception(); }

    auto return_void() noexcept -> void {}

    auto result() -> void
    {
        if (m_exception_ptr)
        {
            std::rethrow_exception(m_exception_ptr);
        }
    }

    auto start(when_all_latch& latch) -> void
    {
        m_latch = &latch;
        coroutine_handle_type::from_promise(*this).resume();
    }

private:
    when_all_latch*    m_latch{nullptr};
    std::exception_ptr m_exception_ptr;
};

template<typename return_type>
class when_all_task
{
public:
    // To be able to call start().
    template<typename task_container_type>
    friend class when_all_ready_awaitable;

    using promise_type          = when_all_task_promise<return_type>;
    using coroutine_handle_type = typename promise_type::coroutine_handle_type;

    when_all_task(coroutine_handle_type coroutine) noexcept : m_coroutine(coroutine) {}

    when_all_task(const when_all_task&) = delete;
    when_all_task(when_all_task&& other) noexcept
        : m_coroutine(std::exchange(other.m_coroutine, coroutine_handle_type{}))
    {
    }

    auto operator=(const when_all_task&) -> when_all_task& = delete;
    auto operator=(when_all_task&&) -> when_all_task&      = delete;

    ~when_all_task()
    {
        if (m_coroutine != nullptr)
        {
            m_coroutine.destroy();
        }
    }

    auto return_value() & -> decltype(auto)
    {
        if constexpr (std::is_void_v<return_type>)
        {
            m_coroutine.promise().result();
            return void_value{};
        }
        else
        {
            return m_coroutine.promise().return_value();
        }
    }

    auto return_value() const& -> decltype(auto)
    {
        if constexpr (std::is_void_v<return_type>)
        {
            m_coroutine.promise().result();
            return void_value{};
        }
        else
        {
            return m_coroutine.promise().return_value();
        }
    }

    auto return_value() && -> decltype(auto)
    {
        if constexpr (std::is_void_v<return_type>)
        {
            m_coroutine.promise().result();
            return void_value{};
        }
        else
        {
            return m_coroutine.promise().return_value();
        }
    }

private:
    auto start(when_all_latch& latch) noexcept -> void { m_coroutine.promise().start(latch); }

    coroutine_handle_type m_coroutine;
};

inline auto make_when_all_task(task<>& awaitable) -> when_all_task<void>
{
    co_await awaitable;
    co_return;
}

} // namespace detail

[[nodiscard]] auto when_all(std::span<task<>> tasks)
    -> detail::when_all_ready_awaitable<std::vector<detail::when_all_task<void>>>
{
    std::vector<detail::when_all_task<void>> output_tasks;

    // If the size is known in constant time reserve the output tasks size.
    output_tasks.reserve(std::size(tasks));

    // Wrap each task into a when_all_task.
    for (auto& task : tasks)
    {
        output_tasks.emplace_back(detail::make_when_all_task(task));
    }

    // Return the single awaitable that drives all the user's tasks.
    return detail::when_all_ready_awaitable(std::move(output_tasks));
}

} // namespace coro

inline auto abstract_task_node::schedule() -> coro::task<> {
    fmt::println("scheduling {}", task_name());
    std::vector<coro::task<>> tasks;
    for (auto& predecessor: predecessors()) {
        tasks.emplace_back(predecessor->schedule());
    }
    co_await coro::when_all(tasks);
    co_await try_schedule_self();
}