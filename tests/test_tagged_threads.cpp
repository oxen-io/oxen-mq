#include "oxenmq/batch.h"
#include "common.h"
#include <future>

TEST_CASE("tagged thread start functions", "[tagged][start]") {
    oxenmq::OxenMQ omq{get_logger(""), LogLevel::trace};

    omq.set_general_threads(2);
    omq.set_batch_threads(2);
    auto t_abc = omq.add_tagged_thread("abc");
    std::atomic<bool> start_called = false;
    auto t_def = omq.add_tagged_thread("def", [&] { start_called = true; });

    std::this_thread::sleep_for(20ms);
    {
        auto lock = catch_lock();
        REQUIRE_FALSE( start_called );
    }

    omq.start();
    wait_for([&] { return start_called.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( start_called );
    }
}

TEST_CASE("tagged threads quit-before-start", "[tagged][quit]") {
    auto omq = std::make_unique<oxenmq::OxenMQ>(get_logger(""), LogLevel::trace);
    auto t_abc = omq->add_tagged_thread("abc");
    REQUIRE_NOTHROW(omq.reset());
}

TEST_CASE("batch jobs to tagged threads", "[tagged][batch]") {
    oxenmq::OxenMQ omq{get_logger(""), LogLevel::trace};

    omq.set_general_threads(2);
    omq.set_batch_threads(2);
    std::thread::id id_abc, id_def;
    auto t_abc = omq.add_tagged_thread("abc", [&] { id_abc = std::this_thread::get_id(); });
    auto t_def = omq.add_tagged_thread("def", [&] { id_def = std::this_thread::get_id(); });
    omq.start();

    std::atomic<bool> done = false;
    std::thread::id id;
    omq.job([&] { id = std::this_thread::get_id(); done = true; });
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( id != id_abc );
        REQUIRE( id != id_def );
    }
    
    done = false;
    omq.job([&] { id = std::this_thread::get_id(); done = true; }, t_abc);
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( id == id_abc );
    }

    done = false;
    omq.job([&] { id = std::this_thread::get_id(); done = true; }, t_def);
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( id == id_def );
    }

    std::atomic<bool> sleep = true;
    auto sleeper = [&] { for (int i = 0; sleep && i < 10; i++) { std::this_thread::sleep_for(25ms); } };
    omq.job(sleeper);
    omq.job(sleeper);
    // This one should stall:
    std::atomic<bool> bad = false;
    omq.job([&] { bad = true; });

    std::this_thread::sleep_for(50ms);

    done = false;
    omq.job([&] { id = std::this_thread::get_id(); done = true; }, t_abc);
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( done.load() );
        REQUIRE_FALSE( bad.load() );
    }

    done = false;
    // We can queue up a bunch of jobs which should all happen in order, and all on the abc thread.
    std::vector<int> v;
    for (int i = 0; i < 100; i++) {
        omq.job([&] { if (std::this_thread::get_id() == id_abc) v.push_back(v.size()); }, t_abc);
    }
    omq.job([&] { done = true; }, t_abc);
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( done.load() );
        REQUIRE_FALSE( bad.load() );
        REQUIRE( v.size() == 100 );
        for (int i = 0; i < 100; i++)
            REQUIRE( v[i] == i );
    }
    sleep = false;
    wait_for([&] { return bad.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( bad.load() );
    }
}

TEST_CASE("batch job completion on tagged threads", "[tagged][batch-completion]") {
    oxenmq::OxenMQ omq{get_logger(""), LogLevel::trace};

    omq.set_general_threads(4);
    omq.set_batch_threads(4);
    std::thread::id id_abc;
    auto t_abc = omq.add_tagged_thread("abc", [&] { id_abc = std::this_thread::get_id(); });
    omq.start();

    oxenmq::Batch<int> batch;
    for (int i = 1; i < 10; i++)
        batch.add_job([i, &id_abc]() { if (std::this_thread::get_id() == id_abc) return 0; return i; });

    std::atomic<int> result_sum = -1;
    batch.completion([&](auto result) {
        int sum = 0;
        for (auto& r : result)
            sum += r.get();
        result_sum = std::this_thread::get_id() == id_abc ? sum : -sum;
    }, t_abc);
    omq.batch(std::move(batch));
    wait_for([&] { return result_sum.load() != -1; });
    {
        auto lock = catch_lock();
        REQUIRE( result_sum == 45 );
    }
}


TEST_CASE("timer job completion on tagged threads", "[tagged][timer]") {
    oxenmq::OxenMQ omq{get_logger(""), LogLevel::trace};

    omq.set_general_threads(4);
    omq.set_batch_threads(4);

    std::thread::id id_abc;
    auto t_abc = omq.add_tagged_thread("abc", [&] { id_abc = std::this_thread::get_id(); });
    omq.start();

    std::atomic<int> ticks = 0;
    std::atomic<int> abc_ticks = 0;
    omq.add_timer([&] { ticks++; }, 10ms);
    omq.add_timer([&] { if (std::this_thread::get_id() == id_abc) abc_ticks++; }, 10ms, true, t_abc);

    wait_for([&] { return ticks.load() > 2 && abc_ticks > 2; });
    {
        auto lock = catch_lock();
        REQUIRE( ticks.load() > 2 );
        REQUIRE( abc_ticks.load() > 2 );
    }
}


