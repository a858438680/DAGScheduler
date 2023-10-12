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

} // namespace coro