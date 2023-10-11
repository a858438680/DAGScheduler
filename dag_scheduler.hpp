#pragma once

#include <memory>
#include <print>
#include <unordered_set>
#include <unordered_map>

#include "task.hpp"
#include "thread_pool.hpp"
#include "abstract_task.hpp"

namespace coro {

struct task_node: public abstract_task_node {
    coro::task<> task_;
    std::vector<std::shared_ptr<abstract_task_node>> predecessors_;
    int i;

    task_node(int i, coro::task<> task, std::vector<std::shared_ptr<abstract_task_node>> predecessors)
    : i(i), task_(std::move(task))
    , predecessors_(std::move(predecessors)) {}

    auto task_name() -> std::string override { return std::format("task{}", i); }
    auto task() -> coro::task<>& override { return task_; }
    auto predecessors() -> std::span<std::shared_ptr<abstract_task_node>> override { return predecessors_; }
};

struct scheduler_t {
    constexpr scheduler_t() noexcept = default;
};

inline scheduler_t scheduler;

// class dag_scheduler {
// public:
//     dag_scheduler(coro::thread_pool& thread_pool): m_thread_pool(thread_pool) {}

//     auto schedule(const std::shared_ptr<abstract_task_node>& node)  {
//         struct awaitable {
//             auto await_ready() const noexcept -> bool { return false; }
//             auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> void {
//                 std::lock_guard lock(self_.m_mutex);
//                 self_.m_awaiting_coroutines.emplace(node_, awaiting_coroutine);
//                 self_.m_submission_queue.emplace_back(node_);
//                 self_.m_cv.notify_one();
//             }
//             auto await_resume() noexcept -> void {}

//             dag_scheduler& self_;
//             const std::shared_ptr<abstract_task_node>& node_;
//         };

//         return awaitable{*this, node};
//     }

// private:
//     struct completion_token {
//         std::shared_ptr<abstract_task_node> node;
//     };

//     coro::thread_pool& m_thread_pool;
//     std::mutex m_mutex;
//     std::condition_variable m_cv;
//     std::deque<std::shared_ptr<abstract_task_node>> m_submission_queue;
//     std::deque<completion_token> m_completion_queue;
//     std::unordered_map<std::shared_ptr<abstract_task_node>, size_t> m_waiting_tasks_to_completion_count;
//     std::unordered_map<std::shared_ptr<abstract_task_node>, std::unordered_set<std::shared_ptr<abstract_task_node>>> m_dependencies_to_waiting_tasks;
//     std::unordered_map<std::shared_ptr<abstract_task_node>, std::coroutine_handle<>> m_awaiting_coroutines;

//     auto run() -> void {
//         while (true) {
//             std::unique_lock lock(m_mutex);
//             m_cv.wait(lock, [this] { return not m_submission_queue.empty() or not m_completion_queue.empty(); });
//             while (not m_submission_queue.empty()) {
//                 auto node = m_submission_queue.front();
//                 m_submission_queue.pop_front();
//                 if (m_waiting_tasks_to_completion_count.contains(node)) {
//                     continue;
//                 }
//                 m_waiting_tasks_to_completion_count.emplace(node, node->predecessors().size());
//                 for (auto& predecessor: node->predecessors()) {
//                     if (auto itr = m_dependencies_to_waiting_tasks.find(predecessor); itr != m_dependencies_to_waiting_tasks.end()) {
//                         itr->second.emplace(node);
//                     } else {
//                         m_dependencies_to_waiting_tasks.emplace(predecessor, std::unordered_set<std::shared_ptr<abstract_task_node>>{node});
//                     }
//                     m_submission_queue.emplace_back(predecessor);
//                 }
//                 if (node->predecessors().empty()) {
                    
//                     m_thread_pool.resume()
//                 }
//             }
//         }
//     }

//     auto notify_task(std::shared_ptr<abstract_task_node> node) -> task<> {
//         co_await node->task();
        
//     }
// };

inline auto detail::promise_base::set_scheduler(dag_scheduler& scheduler) noexcept -> void {
    m_scheduler = &scheduler;
}

inline auto detail::promise_base::await_transform(scheduler_t) noexcept {
    struct awaitable {
        auto await_ready() const noexcept -> bool { return true; }
        auto await_suspend(std::coroutine_handle<>) noexcept -> void {}
        auto await_resume() noexcept -> dag_scheduler& { return *this_->m_scheduler; }

        promise_base* this_;
    };

    return awaitable{this};
}

} // namespace coro