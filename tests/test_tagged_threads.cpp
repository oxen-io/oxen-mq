#include "lokimq/batch.h"
#include "common.h"
#include <future>

TEST_CASE("tagged thread start functions", "[tagged][start]") {
    lokimq::LokiMQ lmq{get_logger(""), LogLevel::trace};

    lmq.set_general_threads(2);
    lmq.set_batch_threads(2);
    auto t_abc = lmq.add_tagged_thread("abc");
    std::atomic<bool> start_called = false;
    auto t_def = lmq.add_tagged_thread("def", [&] { start_called = true; });

    std::this_thread::sleep_for(20ms);
    {
        auto lock = catch_lock();
        REQUIRE_FALSE( start_called );
    }

    lmq.start();
    wait_for([&] { return start_called.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( start_called );
    }
}

TEST_CASE("tagged threads quit-before-start", "[tagged][quit]") {
    auto lmq = std::make_unique<lokimq::LokiMQ>(get_logger(""), LogLevel::trace);
    auto t_abc = lmq->add_tagged_thread("abc");
    REQUIRE_NOTHROW(lmq.reset());
}

TEST_CASE("batch jobs to tagged threads", "[tagged][batch]") {
    lokimq::LokiMQ lmq{get_logger(""), LogLevel::trace};

    lmq.set_general_threads(2);
    lmq.set_batch_threads(2);
    std::thread::id id_abc, id_def;
    auto t_abc = lmq.add_tagged_thread("abc", [&] { id_abc = std::this_thread::get_id(); });
    auto t_def = lmq.add_tagged_thread("def", [&] { id_def = std::this_thread::get_id(); });
    lmq.start();

    std::atomic<bool> done = false;
    std::thread::id id;
    lmq.job([&] { id = std::this_thread::get_id(); done = true; });
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( id != id_abc );
        REQUIRE( id != id_def );
    }
    
    done = false;
    lmq.job([&] { id = std::this_thread::get_id(); done = true; }, t_abc);
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( id == id_abc );
    }

    done = false;
    lmq.job([&] { id = std::this_thread::get_id(); done = true; }, t_def);
    wait_for([&] { return done.load(); });
    {
        auto lock = catch_lock();
        REQUIRE( id == id_def );
    }

    std::atomic<bool> sleep = true;
    auto sleeper = [&] { for (int i = 0; sleep && i < 10; i++) { std::this_thread::sleep_for(25ms); } };
    lmq.job(sleeper);
    lmq.job(sleeper);
    // This one should stall:
    std::atomic<bool> bad = false;
    lmq.job([&] { bad = true; });

    std::this_thread::sleep_for(50ms);

    done = false;
    lmq.job([&] { id = std::this_thread::get_id(); done = true; }, t_abc);
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
        lmq.job([&] { if (std::this_thread::get_id() == id_abc) v.push_back(v.size()); }, t_abc);
    }
    lmq.job([&] { done = true; }, t_abc);
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
    lokimq::LokiMQ lmq{get_logger(""), LogLevel::trace};

    lmq.set_general_threads(4);
    lmq.set_batch_threads(4);
    std::thread::id id_abc;
    auto t_abc = lmq.add_tagged_thread("abc", [&] { id_abc = std::this_thread::get_id(); });
    lmq.start();

    lokimq::Batch<int> batch;
    for (int i = 1; i < 10; i++)
        batch.add_job([i, &id_abc]() { if (std::this_thread::get_id() == id_abc) return 0; return i; });

    std::atomic<int> result_sum = -1;
    batch.completion([&](auto result) {
        int sum = 0;
        for (auto& r : result)
            sum += r.get();
        result_sum = std::this_thread::get_id() == id_abc ? sum : -sum;
    }, t_abc);
    lmq.batch(std::move(batch));
    wait_for([&] { return result_sum.load() != -1; });
    {
        auto lock = catch_lock();
        REQUIRE( result_sum == 45 );
    }
}


TEST_CASE("timer job completion on tagged threads", "[tagged][timer]") {
    lokimq::LokiMQ lmq{get_logger(""), LogLevel::trace};

    lmq.set_general_threads(4);
    lmq.set_batch_threads(4);

    std::thread::id id_abc;
    auto t_abc = lmq.add_tagged_thread("abc", [&] { id_abc = std::this_thread::get_id(); });
    lmq.start();

    std::atomic<int> ticks = 0;
    std::atomic<int> abc_ticks = 0;
    lmq.add_timer([&] { ticks++; }, 10ms);
    lmq.add_timer([&] { if (std::this_thread::get_id() == id_abc) abc_ticks++; }, 10ms, true, t_abc);

    wait_for([&] { return ticks.load() > 2 && abc_ticks > 2; });
    {
        auto lock = catch_lock();
        REQUIRE( ticks.load() > 2 );
        REQUIRE( abc_ticks.load() > 2 );
    }
}


