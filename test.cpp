#include <print>
#include "task.hpp"
#include "thread_pool.hpp"
#include "sync_wait.hpp"
#include "dag_scheduler.hpp"

struct job_task: public abstract_task_node {
    coro::task<> task_;

    auto task_name() -> std::string override { return "job_task"; }
    auto task() -> coro::task<>& override { return task_; }
    auto predecessors() -> std::span<std::shared_ptr<abstract_task_node>> override { return {}; }

    job_task(coro::task<> task): task_(std::move(task)) {}
};

auto subtask1(int& value) -> coro::task<> {
    value = 42;
    co_return;
}

auto subtask2(std::string& str) -> coro::task<> {
    str = "the answer to the world is";
    co_return;
}

#define JOB(X) std::dynamic_pointer_cast<abstract_task_node>(std::make_shared<job_task>(X))

auto task1() -> coro::task<> {
    std::println("1");
    co_return;
}

auto task2() -> coro::task<> {
    std::println("2");
    co_return;
}

auto task3() -> coro::task<> {
    std::println("3");
    co_return;
}

auto task4() -> coro::task<> {
    std::println("4");
    co_return;
}

auto main() -> int {
    coro::thread_pool tp;

#define NODE(X, ...) std::dynamic_pointer_cast<abstract_task_node>(std::make_shared<coro::task_node>(X, std::move(task##X()), std::vector<std::shared_ptr<abstract_task_node>>{__VA_ARGS__}))

    auto node4 = NODE(4);
    auto node3 = NODE(3, node4);
    auto node2 = NODE(2, node3, node4);
    auto node1 = NODE(1, node2, node3);

    coro::sync_wait(node1.schedule(tp));
}