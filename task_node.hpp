#pragma once

#include <memory>
#include <format>

#include <coro/task.hpp>
#include <coro/thread_pool.hpp>
#include <fmt/core.h>

#include "abstract_task.hpp"

struct task_node: public abstract_task_node {
    coro::task<> task_;
    std::vector<std::shared_ptr<abstract_task_node>> predecessors_;
    int i;

    task_node(int i, coro::task<> task, std::vector<std::shared_ptr<abstract_task_node>> predecessors)
    : task_(std::move(task))
    , predecessors_(std::move(predecessors))
    , i(i) {}

    auto task_name() -> std::string override { return std::format("task{}", i); }
    auto task() -> coro::task<>& override { return task_; }
    auto predecessors() -> std::span<std::shared_ptr<abstract_task_node>> override { return predecessors_; }
};