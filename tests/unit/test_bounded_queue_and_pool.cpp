#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "codelenses/index/bounded_queue.hpp"
#include "codelenses/index/incremental_planner.hpp"
#include "codelenses/index/thread_pool.hpp"

using namespace codelenses;
using namespace codelenses::index;

TEST_CASE("BoundedQueue enforces limits and handles cancellation (D-07)", "[index][queue]") {
    SECTION("Queue rejects non-positive limits on creation") {
        using Q = BoundedQueue<std::string>;
        REQUIRE_THROWS_AS(Q(Q::Limits{0, 1024}), std::invalid_argument);
        REQUIRE_THROWS_AS(Q(Q::Limits{10, 0}), std::invalid_argument);
    }

    SECTION("Queue pushes and pops in FIFO order") {
        BoundedQueue<int> q(BoundedQueue<int>::Limits{5, 1024}, [](int) { return sizeof(int); });
        REQUIRE(q.push(1) == QueuePushResult::pushed);
        REQUIRE(q.push(2) == QueuePushResult::pushed);
        REQUIRE(q.push(3) == QueuePushResult::pushed);
        REQUIRE(q.size() == 3);

        auto val1 = q.pop();
        REQUIRE(val1.has_value());
        REQUIRE(*val1 == 1);

        auto val2 = q.pop();
        REQUIRE(val2.has_value());
        REQUIRE(*val2 == 2);

        auto val3 = q.pop();
        REQUIRE(val3.has_value());
        REQUIRE(*val3 == 3);
        REQUIRE(q.size() == 0);
    }

    SECTION("Queue rejects items exceeding max bytes") {
        BoundedQueue<std::string> q(BoundedQueue<std::string>::Limits{5, 10},
                                    [](const std::string& s) { return s.size(); });
        REQUIRE(q.push("short") == QueuePushResult::pushed);
        REQUIRE(q.push("too long string exceeding limit") == QueuePushResult::too_large);
    }

    SECTION("Non-blocking try_push and try_pop") {
        BoundedQueue<int> q(BoundedQueue<int>::Limits{2, 1024}, [](int) { return sizeof(int); });
        REQUIRE(q.try_push(10) == QueuePushResult::pushed);
        REQUIRE(q.try_push(20) == QueuePushResult::pushed);
        REQUIRE(q.try_push(30) == QueuePushResult::full);

        auto p1 = q.try_pop();
        REQUIRE(p1.has_value());
        REQUIRE(*p1 == 10);

        auto p2 = q.try_pop();
        REQUIRE(p2.has_value());
        REQUIRE(*p2 == 20);

        auto p3 = q.try_pop();
        REQUIRE_FALSE(p3.has_value());
    }

    SECTION("Queue cancellation unblocks waiting threads immediately") {
        BoundedQueue<int> q(BoundedQueue<int>::Limits{1, 1024}, [](int) { return sizeof(int); });
        std::atomic<bool> pop_unblocked{false};

        std::thread consumer([&] {
            auto res = q.pop();
            REQUIRE_FALSE(res.has_value());
            pop_unblocked = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        REQUIRE_FALSE(pop_unblocked);

        q.cancel();
        consumer.join();
        REQUIRE(pop_unblocked);
        REQUIRE(q.cancelled());
    }

    SECTION("Queue graceful close drains existing items") {
        BoundedQueue<int> q(BoundedQueue<int>::Limits{5, 1024}, [](int) { return sizeof(int); });
        REQUIRE(q.push(1) == QueuePushResult::pushed);
        REQUIRE(q.push(2) == QueuePushResult::pushed);
        q.close();

        REQUIRE(q.closed());
        REQUIRE(q.push(3) == QueuePushResult::closed);

        auto v1 = q.pop();
        REQUIRE(v1 == 1);
        auto v2 = q.pop();
        REQUIRE(v2 == 2);
        auto v3 = q.pop();
        REQUIRE_FALSE(v3.has_value());
    }
}

TEST_CASE("ThreadPool executes tasks concurrently and handles shutdown (D-07)", "[index][pool]") {
    SECTION("ThreadPool executes tasks and returns futures") {
        ThreadPool pool(4);
        std::vector<std::future<int>> futures;

        for (int i = 0; i < 10; ++i) {
            futures.push_back(pool.submit([i] { return i * 2; }));
        }

        for (std::size_t i = 0; i < 10; ++i) {
            REQUIRE(futures[i].get() == static_cast<int>(i) * 2);
        }
    }

    SECTION("ThreadPool shutdown waits for running tasks") {
        ThreadPool pool(2);
        std::atomic<int> counter{0};

        for (int i = 0; i < 5; ++i) {
            pool.submit_task([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                counter++;
            });
        }

        pool.shutdown();
        REQUIRE(counter == 5);
        REQUIRE(pool.is_stopped());
    }
}

TEST_CASE("Incremental planner classifies files correctly (D-05, D-10)", "[index][planner]") {
    std::vector<filesystem::DiscoveredFile> discovered = {
        {.relative_path = "src/unchanged.cpp",
         .absolute_path = "/ws/src/unchanged.cpp",
         .language = Language::cpp,
         .file_size = 100},
        {.relative_path = "src/modified.cpp",
         .absolute_path = "/ws/src/modified.cpp",
         .language = Language::cpp,
         .file_size = 200},
        {.relative_path = "src/new.cpp",
         .absolute_path = "/ws/src/new.cpp",
         .language = Language::cpp,
         .file_size = 50},
    };

    std::vector<FileStateItem> db_states = {
        {.id = 1,
         .relative_path = "src/unchanged.cpp",
         .size_bytes = 100,
         .modified_ns = 0,
         .is_deleted = false},
        {.id = 2,
         .relative_path = "src/modified.cpp",
         .size_bytes = 150, // different size!
         .modified_ns = 0,
         .is_deleted = false},
        {.id = 3,
         .relative_path = "src/deleted.cpp",
         .size_bytes = 300,
         .modified_ns = 0,
         .is_deleted = false},
    };

    auto plan = plan_indexing(1, discovered, db_states, false);

    REQUIRE(plan.files_to_skip.size() == 1);
    REQUIRE(plan.files_to_skip[0].relative_path == "src/unchanged.cpp");

    REQUIRE(plan.files_to_process.size() == 2);
    // modified and new
    bool has_mod = false;
    bool has_new = false;
    for (const auto& pf : plan.files_to_process) {
        if (pf.relative_path == "src/modified.cpp") {
            has_mod = true;
            REQUIRE(pf.file_id == 2);
        }
        if (pf.relative_path == "src/new.cpp") {
            has_new = true;
            REQUIRE(pf.file_id == 0);
        }
    }
    REQUIRE(has_mod);
    REQUIRE(has_new);

    // deleted file identified
    REQUIRE(plan.file_ids_to_remove.size() == 1);
    REQUIRE(plan.file_ids_to_remove[0] == 3);

    // force_full reindexes all files
    auto full_plan = plan_indexing(1, discovered, db_states, true);
    REQUIRE(full_plan.files_to_skip.empty());
    REQUIRE(full_plan.files_to_process.size() == 3);
}
