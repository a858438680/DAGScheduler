#pragma once

#include <type_traits>
#include <coroutine>

namespace coro {

namespace concepts {

template <typename T, typename... Ts>
concept in_types = (std::is_same_v<T, Ts> || ...);

template <typename T>
concept awaiter = requires(T t, std::coroutine_handle<> c) {
    { t.await_ready() } -> std::convertible_to<bool>;
    { t.await_suspend(c) } -> in_types<void, bool, std::coroutine_handle<>>;
    { t.await_resume() };
};

template <typename T>
concept awaitable = requires(T t) {
    { t.operator co_await() } -> awaiter;
} || requires(T t) {
    { operator co_await(t) } -> awaiter;
} || awaiter<T>;

template <typename T>
struct awaitable_traits {
    using awaiter_return_type = decltype(std::declval<T>().operator co_await().await_resume());
};

} // namespace concepts

} // namespace coro