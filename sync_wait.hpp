#pragma once

#include <coroutine>
#include <condition_variable>
#include <exception>
#include <mutex>

#include "concepts/awaitable.hpp"

namespace coro {

namespace detail {

class sync_wait_event {
public:
    sync_wait_event(bool initial_set = false): m_set(initial_set) {}
    sync_wait_event(const sync_wait_event&) = delete;
    sync_wait_event(sync_wait_event&&) = delete;
    auto operator=(const sync_wait_event&) -> sync_wait_event& = delete;
    auto operator=(sync_wait_event&&) -> sync_wait_event& = delete;
    ~sync_wait_event() = default;

    auto set() -> void {
        {
            std::lock_guard lock(m_mutex);
            m_set = true;
        }

        m_cv.notify_all();
    }

    auto reset() -> void {
        std::lock_guard lock(m_mutex);
        m_set = false;
    }

    auto wait() -> void {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_set; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_set;
};

class sync_wait_task_promise_base {
public:
    sync_wait_task_promise_base() noexcept = default;
    ~sync_wait_task_promise_base() = default;

    template <typename Self>
    auto start(this Self& self, sync_wait_event& event) -> void {
        self.m_event = &event;
        std::remove_reference_t<Self>::coroutine_handle::from_promise(self).resume();
    }

    template <typename Self>
    auto get_return_object(this Self& self) noexcept { return std::remove_reference_t<Self>::coroutine_handle::from_promise(self); }

    auto initial_suspend() noexcept -> std::suspend_always { return {}; };

    template <typename Self>
    auto final_suspend(this Self& self) noexcept {
        struct completion_notifier {
            auto await_ready() const noexcept -> bool { return false; }
            auto await_suspend(std::remove_reference_t<Self>::coroutine_handle awaiting_coroutine) const noexcept {
                awaiting_coroutine.promise().m_event->set();
            }
            auto await_resume() noexcept -> void {}
        };

        return completion_notifier{};
    }

    auto unhandled_exception() noexcept -> void { m_exception_ptr = std::current_exception(); }

    auto return_void() noexcept -> void {}

protected:
    sync_wait_event* m_event{};
    std::exception_ptr m_exception_ptr;
};

template <typename return_type>
class sync_wait_task_promise: public sync_wait_task_promise_base {
public:
    using coroutine_handle = std::coroutine_handle<sync_wait_task_promise<return_type>>;

    sync_wait_task_promise() noexcept = default;
    ~sync_wait_task_promise() = default;

    auto yield_value(return_type&& value) noexcept {
        m_return_value = std::addressof(value);
        return final_suspend();
    }

    auto result() -> return_type&& {
        if (m_exception_ptr) {
            std::rethrow_exception(m_exception_ptr);
        }

        return static_cast<return_type&&>(*m_return_value);
    }

private:
    std::remove_reference_t<return_type>* m_return_value{};
};

template <>
class sync_wait_task_promise<void>: public sync_wait_task_promise_base {
public:
    using coroutine_handle = std::coroutine_handle<sync_wait_task_promise<void>>;

    sync_wait_task_promise() noexcept = default;
    ~sync_wait_task_promise() = default;

    auto result() -> void {
        if (m_exception_ptr) {
            std::rethrow_exception(m_exception_ptr);
        }
    }
};

template <typename return_type>
class sync_wait_task {
public:
    using promise_type = sync_wait_task_promise<return_type>;
    using coroutine_handle = std::coroutine_handle<promise_type>;

    sync_wait_task(coroutine_handle handle): m_coroutine(handle) {}
    sync_wait_task(const sync_wait_task&) = delete;
    sync_wait_task(sync_wait_task&& other) noexcept: m_coroutine(std::exchange(other.m_coroutine, nullptr)) {}
    ~sync_wait_task() {
        if (m_coroutine != nullptr) {
            m_coroutine.destroy();
        }
    }

    auto operator=(const sync_wait_task&) -> sync_wait_task& = delete;
    auto operator=(sync_wait_task&& other) noexcept -> sync_wait_task& {
        if (std::addressof(other) != this) {
            if (m_coroutine != nullptr) {
                m_coroutine.destroy();
            }

            m_coroutine = std::exchange(other.m_coroutine, nullptr);
        }

        return *this;
    }

    auto start(sync_wait_event& event) noexcept -> void {
        m_coroutine.promise().start(event);
    }

    auto return_value() noexcept -> return_type {
        return m_coroutine.promise().result();
    }
private:
    coroutine_handle m_coroutine;
};

template <concepts::awaitable awaitable_type>
static auto make_sync_wait_task(awaitable_type&& a) ->
    sync_wait_task<typename concepts::awaitable_traits<awaitable_type>::awaiter_return_type> {
    using return_type = concepts::awaitable_traits<awaitable_type>::awaiter_return_type;
    if constexpr (std::is_void_v<return_type>) {
        co_await std::forward<awaitable_type>(a);
        co_return;
    } else {
        co_yield co_await std::forward<awaitable_type>(a);
    }
}

} // namespace detail

template <concepts::awaitable awaitable_type>
auto sync_wait(awaitable_type&& a) -> decltype(auto) {
    detail::sync_wait_event e{};
    auto task = detail::make_sync_wait_task(std::forward<awaitable_type>(a));
    task.start(e);
    e.wait();

    return task.return_value();
}

} // namespace coro