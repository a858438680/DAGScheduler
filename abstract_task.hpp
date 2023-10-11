#pragma once

#include <memory>
#include <string>
#include <span>
#include <vector>
#include "task.hpp"
#include <thread>

struct abstract_task_node {
    virtual auto task_name() -> std::string = 0;
    virtual auto task() -> coro::task<>& = 0;
    virtual auto predecessors() -> std::span<std::shared_ptr<abstract_task_node>> = 0;
    auto schedule() -> coro::task<>;
    virtual ~abstract_task_node() = default;
};

namespace coro {

namespace detail {

struct void_value {};

struct when_all_latch {
public:
    when_all_latch(std::size_t count): m_count(count) {}
    when_all_latch(const when_all_latch&) = delete;
    when_all_latch(when_all_latch&&) = delete;
    auto operator=(const when_all_latch&) -> when_all_latch& = delete;
    auto operator=(when_all_latch&&) -> when_all_latch& = delete;

    auto is_ready() const noexcept -> bool { return m_awaiting_coroutine == nullptr || m_awaiting_coroutine.done(); }
    auto try_await(std::coroutine_handle<> awaiting_coroutine) -> bool {
        m_awaiting_coroutine = awaiting_coroutine;
        return m_count.fetch_sub(1, std::memory_order_acq_rel) > 0;
    }
    auto notify_awaitable_completed() noexcept -> void {
        if (m_count.fetch_sub(1, std::memory_order_acq_rel) == 0) {
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

/// Empty tuple<> implementation.
template<>
class when_all_ready_awaitable<std::tuple<>>
{
public:
    constexpr when_all_ready_awaitable() noexcept {}
    explicit constexpr when_all_ready_awaitable(std::tuple<>) noexcept {}

    constexpr auto await_ready() const noexcept -> bool { return true; }
    auto           await_suspend(std::coroutine_handle<>) noexcept -> void {}
    auto           await_resume() const noexcept -> std::tuple<> { return {}; }
};

template<typename... task_types>
class when_all_ready_awaitable<std::tuple<task_types...>>
{
public:
    explicit when_all_ready_awaitable(task_types&&... tasks) noexcept(
        std::conjunction<std::is_nothrow_move_constructible<task_types>...>::value)
        : m_latch(sizeof...(task_types)),
          m_tasks(std::move(tasks)...)
    {
    }

    explicit when_all_ready_awaitable(std::tuple<task_types...>&& tasks) noexcept(
        std::is_nothrow_move_constructible_v<std::tuple<task_types...>>)
        : m_latch(sizeof...(task_types)),
          m_tasks(std::move(tasks))
    {
    }

    when_all_ready_awaitable(const when_all_ready_awaitable&) = delete;
    when_all_ready_awaitable(when_all_ready_awaitable&& other)
        : m_latch(std::move(other.m_latch)),
          m_tasks(std::move(other.m_tasks))
    {
    }

    auto operator=(const when_all_ready_awaitable&) -> when_all_ready_awaitable& = delete;
    auto operator=(when_all_ready_awaitable&&) -> when_all_ready_awaitable&      = delete;

    auto operator co_await() & noexcept
    {
        struct awaiter
        {
            explicit awaiter(when_all_ready_awaitable& awaitable) noexcept : m_awaitable(awaitable) {}

            auto await_ready() const noexcept -> bool { return m_awaitable.is_ready(); }

            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
            {
                return m_awaitable.try_await(awaiting_coroutine);
            }

            auto await_resume() noexcept -> std::tuple<task_types...>& { return m_awaitable.m_tasks; }

        private:
            when_all_ready_awaitable& m_awaitable;
        };

        return awaiter{*this};
    }

    auto operator co_await() && noexcept
    {
        struct awaiter
        {
            explicit awaiter(when_all_ready_awaitable& awaitable) noexcept : m_awaitable(awaitable) {}

            auto await_ready() const noexcept -> bool { return m_awaitable.is_ready(); }

            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
            {
                return m_awaitable.try_await(awaiting_coroutine);
            }

            auto await_resume() noexcept -> std::tuple<task_types...>&& { return std::move(m_awaitable.m_tasks); }

        private:
            when_all_ready_awaitable& m_awaitable;
        };

        return awaiter{*this};
    }

private:
    auto is_ready() const noexcept -> bool { return m_latch.is_ready(); }

    auto try_await(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
    {
        std::apply([this](auto&&... tasks) { ((tasks.start(m_latch)), ...); }, m_tasks);
        return m_latch.try_await(awaiting_coroutine);
    }

    when_all_latch            m_latch;
    std::tuple<task_types...> m_tasks;
};

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

template<
    concepts::awaitable awaitable,
    typename return_type = typename concepts::awaitable_traits<awaitable&&>::awaiter_return_type>
static auto make_when_all_task(awaitable a) -> when_all_task<return_type> __attribute__((used));

template<concepts::awaitable awaitable, typename return_type>
static auto make_when_all_task(awaitable a) -> when_all_task<return_type>
{
    if constexpr (std::is_void_v<return_type>)
    {
        co_await static_cast<awaitable&&>(a);
        co_return;
    }
    else
    {
        co_yield co_await static_cast<awaitable&&>(a);
    }
}

} // namespace detail

template<concepts::awaitable... awaitables_type>
[[nodiscard]] auto when_all(awaitables_type... awaitables)
{
    return detail::when_all_ready_awaitable<std::tuple<
        detail::when_all_task<typename concepts::awaitable_traits<awaitables_type>::awaiter_return_type>...>>(
        std::make_tuple(detail::make_when_all_task(std::move(awaitables))...));
}

[[nodiscard]] auto when_all(std::span<std::reference_wrapper<task<>>> awaitables)
    -> detail::when_all_ready_awaitable<std::vector<detail::when_all_task<void>>>
{
    std::vector<detail::when_all_task<void>> output_tasks;

    // If the size is known in constant time reserve the output tasks size.
    output_tasks.reserve(std::size(awaitables));

    // Wrap each task into a when_all_task.
    for (auto& a : awaitables)
    {
        output_tasks.emplace_back(detail::make_when_all_task(std::move(a)));
    }

    // Return the single awaitable that drives all the user's tasks.
    return detail::when_all_ready_awaitable(std::move(output_tasks));
}

} // namespace coro

inline auto abstract_task_node::schedule() -> coro::task<> {
    std::vector<std::reference_wrapper<coro::task<>>> tasks;
    for (auto& predecessor: predecessors()) {
        tasks.emplace_back(predecessor->task());
    }
    co_await coro::when_all(tasks);
    co_await task();
}