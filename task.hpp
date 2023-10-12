#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "concepts/awaitable.hpp"

namespace coro {

template <typename return_type = void>
class task;

namespace detail {

struct promise_base {
    friend struct final_awaitable;
    struct final_awaitable {
        auto await_ready() const noexcept -> bool { return false; }

        template <typename promise_type>
        auto await_suspend(std::coroutine_handle<promise_type> coroutine) -> std::coroutine_handle<> {
            auto& promise = coroutine.promise();
            if (promise.m_continuation != nullptr) {
                return promise.m_continuation;
            } else {
                return std::noop_coroutine();
            }
        }

        auto await_resume() noexcept -> void {}
    };

    promise_base() noexcept = default;
    ~promise_base() = default;

    auto initial_suspend() noexcept { return std::suspend_always{}; }
    auto final_suspend() noexcept { return final_awaitable{}; }
    auto unhandled_exception() noexcept -> void { m_exception_ptr = std::current_exception(); }
    auto set_continuation(std::coroutine_handle<> continuation) noexcept { m_continuation = continuation; }

protected:
    std::coroutine_handle<> m_continuation;
    std::exception_ptr m_exception_ptr;
};

template <typename return_type>
struct promise final: public promise_base {
    using task_type = task<return_type>;
    using coroutine_handle = std::coroutine_handle<promise<return_type>>;
    static constexpr bool return_type_is_reference = std::is_reference_v<return_type>;

    promise() noexcept = default;
    ~promise() = default;

    auto get_return_object() noexcept -> task_type;
    auto return_value(return_type value) -> void {
        if constexpr (return_type_is_reference) {
            m_return_value = std::addressof(value);
        } else {
            m_return_value = {std::move(value)};
        }
    }

    template <typename Self>
    auto result(this Self&& self) -> decltype(auto) {
        if (self.m_exception_ptr) {
            std::rethrow_exception(self.m_exception_ptr);
        }

        if constexpr (return_type_is_reference) {
            if (self.m_return_value) {
                return static_cast<return_type>(*self.m_return_value);
            }
        } else {
            if (self.m_return_value.has_value()) {
                return std::forward_like<Self>(self.m_return_value.value());
            }
        }

        throw std::runtime_error("The return value was never set, did you execute the coroutine?");
    }

private:
    using held_type = std::conditional_t<
        return_type_is_reference,
        std::remove_reference_t<return_type>*,
        std::optional<return_type>>;
    
    held_type m_return_value{};
};

template <>
struct promise<void> final: public promise_base {
    using task_type = task<void>;
    using coroutine_handle = std::coroutine_handle<promise<void>>;

    promise() noexcept = default;
    ~promise() = default;

    auto get_return_object() noexcept -> task_type;
    auto return_void() noexcept -> void {}
    auto result() -> void {
        if (m_exception_ptr) {
            std::rethrow_exception(m_exception_ptr);
        }
    }
};

} // namespace detail

template <typename return_type>
class [[nodiscard]] task {
public:
    using task_type = task<return_type>;
    using promise_type = detail::promise<return_type>;
    using coroutine_handle = std::coroutine_handle<promise_type>;

    struct awaitable_base {
        awaitable_base(coroutine_handle coroutine) noexcept: m_coroutine(coroutine) {}
        auto await_ready() const noexcept -> bool { return !m_coroutine || m_coroutine.done(); }
        auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> std::coroutine_handle<> {
            m_coroutine.promise().set_continuation(awaiting_coroutine);
            return m_coroutine;
        }
        
        std::coroutine_handle<promise_type> m_coroutine{nullptr};
    };

    task() noexcept: m_coroutine(nullptr) {}
    explicit task(coroutine_handle handle): m_coroutine(handle) {}
    task(const task&) = delete;
    task(task&& other) noexcept: m_coroutine(std::exchange(other.m_coroutine, nullptr)) {}

    ~task() {
        if (m_coroutine != nullptr) {
            m_coroutine.destroy();
        }
    }

    auto operator=(const task&) -> task& = delete;
    auto operator=(task&& other) noexcept -> task& {
        if (std::addressof(other) != this) {
            if (m_coroutine != nullptr) {
                m_coroutine.destroy();
            }
            m_coroutine = std::exchange(other.m_coroutine, nullptr);
        }
        return *this;
    }

    auto is_ready() const noexcept -> bool { return m_coroutine == nullptr || m_coroutine.done(); }
    auto resume() -> bool {
        if (!m_coroutine.done()) {
            m_coroutine.resume();
        }
        return !m_coroutine.done();
    }

    auto destroy() -> bool {
        if (m_coroutine != nullptr) {
            m_coroutine.destroy();
            m_coroutine = nullptr;
            return true;
        }
        return false;
    }

    template <typename Self>
    auto operator co_await(this Self&& self) noexcept {
        struct awaitable: public awaitable_base {
            auto await_resume() -> decltype(auto) {
                return std::forward_like<Self>(this->m_coroutine.promise()).result();
            }
        };
        return awaitable{self.m_coroutine};
    }

    template <typename Self>
    auto promise(this Self&& self) -> decltype(auto) {
        return std::forward_like<Self>(self.m_coroutine.promise());
    }

    auto handle() -> coroutine_handle { return m_coroutine; }

private:
    coroutine_handle m_coroutine{nullptr};
};

namespace detail {

template <typename return_type>
inline auto promise<return_type>::get_return_object() noexcept -> task<return_type> {
    return task<return_type>{coroutine_handle::from_promise(*this)};
}

inline auto promise<void>::get_return_object() noexcept -> task<void> {
    return task<>{coroutine_handle::from_promise(*this)};
}

} // namespace detail

} // namespace coro