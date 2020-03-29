#include "lokimq.h"
#include "batch.h"
#include "lokimq-internal.h"

namespace lokimq {

void LokiMQ::proxy_batch(detail::Batch* batch) {
    batches.insert(batch);
    const int jobs = batch->size();
    for (int i = 0; i < jobs; i++)
        batch_jobs.emplace(batch, i);
    proxy_skip_one_poll = true;
}

void LokiMQ::job(std::function<void()> f) {
    auto* b = new Batch<void>;
    b->add_job(std::move(f));
    auto* baseptr = static_cast<detail::Batch*>(b);
    detail::send_control(get_control_socket(), "BATCH", bt_serialize(reinterpret_cast<uintptr_t>(baseptr)));
}

void LokiMQ::proxy_schedule_reply_job(std::function<void()> f) {
    auto* b = new Batch<void>;
    b->add_job(std::move(f));
    batches.insert(b);
    reply_jobs.emplace(static_cast<detail::Batch*>(b), 0);
    proxy_skip_one_poll = true;
}

void LokiMQ::proxy_run_batch_jobs(std::queue<batch_job>& jobs, const int reserved, int& active, bool reply) {
    while (!jobs.empty() && static_cast<int>(workers.size()) < max_workers &&
            (active < reserved || active_workers() < general_workers)) {
        proxy_run_worker(get_idle_worker().load(std::move(jobs.front()), reply));
        jobs.pop();
        active++;
    }
}

// Called either within the proxy thread, or before the proxy thread has been created; actually adds
// the timer.  If the timer object hasn't been set up yet it gets set up here.
void LokiMQ::proxy_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch) {
    if (!timers)
        timers.reset(zmq_timers_new());

    int timer_id = zmq_timers_add(timers.get(),
            interval.count(),
            [](int timer_id, void* self) { static_cast<LokiMQ*>(self)->_queue_timer_job(timer_id); },
            this);
    if (timer_id == -1)
        throw zmq::error_t{};
    timer_jobs[timer_id] = std::make_tuple(std::move(job), squelch, false);
}

void LokiMQ::proxy_timer(bt_list_consumer timer_data) {
    std::unique_ptr<std::function<void()>> func{reinterpret_cast<std::function<void()>*>(timer_data.consume_integer<uintptr_t>())};
    auto interval = std::chrono::milliseconds{timer_data.consume_integer<uint64_t>()};
    auto squelch = timer_data.consume_integer<bool>();
    if (!timer_data.is_finished())
        throw std::runtime_error("Internal error: proxied timer request contains unexpected data");
    proxy_timer(std::move(*func), interval, squelch);
}

void LokiMQ::_queue_timer_job(int timer_id) {
    auto it = timer_jobs.find(timer_id);
    if (it == timer_jobs.end()) {
        LMQ_LOG(warn, "Could not find timer job ", timer_id);
        return;
    }
    auto& timer = it->second;
    auto& squelch = std::get<1>(timer);
    auto& running = std::get<2>(timer);
    if (squelch && running) {
        LMQ_LOG(debug, "Not running timer job ", timer_id, " because a job for that timer is still running");
        return;
    }

    auto* b = new Batch<void>;
    b->add_job(std::get<0>(timer));
    if (squelch) {
        running = true;
        b->completion_proxy([this,timer_id](auto results) {
            try { results[0].get(); }
            catch (const std::exception &e) { LMQ_LOG(warn, "timer job ", timer_id, " raised an exception: ", e.what()); }
            catch (...) { LMQ_LOG(warn, "timer job ", timer_id, " raised a non-std exception"); }
            auto it = timer_jobs.find(timer_id);
            if (it != timer_jobs.end())
                std::get<2>(it->second)/*running*/ = false;
        });
    }
    batches.insert(b);
    batch_jobs.emplace(static_cast<detail::Batch*>(b), 0);
    assert(b->size() == 1);
}

void LokiMQ::add_timer(std::function<void()> job, std::chrono::milliseconds interval, bool squelch) {
    if (proxy_thread.joinable()) {
        detail::send_control(get_control_socket(), "TIMER", bt_serialize(bt_list{{
                    detail::serialize_object(std::move(job)),
                    interval.count(),
                    squelch}}));
    } else {
        proxy_timer(std::move(job), interval, squelch);
    }
}

void LokiMQ::TimersDeleter::operator()(void* timers) { zmq_timers_destroy(&timers); }

}
