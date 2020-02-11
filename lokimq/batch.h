// Copyright (c)      2020, The Loki Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once
#include <exception>
#include <functional>
#include <vector>
#include "lokimq.h"

namespace lokimq {

namespace detail {

enum class BatchStatus {
    running, // there are still jobs to run (or running)
    complete, // the batch is complete but still has a completion job to call
    complete_proxy, // same as `complete`, but the completion job should be invoked immediately in the proxy thread (be very careful)
    done // the batch is complete and has no completion function
};

// Virtual base class for Batch<R>
class Batch {
public:
    // Returns the number of jobs in this batch
    virtual size_t size() const = 0;
    // Called in a worker thread to run the job
    virtual void run_job(int i) = 0;
    // Called in the main proxy thread when the worker returns from finishing a job.  The return
    // value tells us whether the current finishing job finishes off the batch: `running` to tell us
    // there are more jobs; `complete` to tell us that the jobs are done but the completion function
    // needs to be called; and `done` to signal that the jobs are done and there is no completion
    // function.
    virtual BatchStatus job_finished() = 0;
    // Called by a worker; not scheduled until all jobs are done.
    virtual void job_completion() = 0;

    virtual ~Batch() = default;
};

}

/**
 * Simple class that can either hold a result or an exception and retrieves the result (or raises
 * the exception) via a .get() method.
 *
 * This is designed to be like a very stripped down version of a std::promise/std::future pair.  We
 * reimplemented it, however, because by ditching all the thread synchronization that promise/future
 * guarantees we can substantially reduce call overhead (by a factor of ~8 according to benchmarking
 * code).  Since LokiMQ's proxy<->worker communication channel already gives us thread that overhead
 * would just be wasted.
 *
 * @tparam R the value type held by the result; must be default constructible.  Note, however, that
 * there are specializations provided for lvalue references types and `void` (which obviously don't
 * satisfy this).
 */
template <typename R, typename SFINAE = void>
class job_result {
    R value;
    std::exception_ptr exc;

public:
    /// Sets the value.  Should be called only once, or not at all if set_exception was called.
    void set_value(R&& v) { value = std::move(v); }

    /// Sets the exception, which will be rethrown when `get()` is called.  Should be called
    /// only once, or not at all if set_value() was called.
    void set_exception(std::exception_ptr e) { exc = std::move(e); }

    /// Retrieves the value.  If an exception was set instead of a value then that exception is
    /// thrown instead.  Note that the interval value is moved out of the held value so you should
    /// not call this multiple times.
    R get() {
        if (exc) std::rethrow_exception(exc);
        return std::move(value);
    }
};

/** job_result specialization for reference types */
template <typename R>
class job_result<R, std::enable_if_t<std::is_lvalue_reference<R>::value>> {
    std::remove_reference_t<R>* value_ptr;
    std::exception_ptr exc;

public:
    void set_value(R v) { value_ptr = &v; }
    void set_exception(std::exception_ptr e) { exc = std::move(e); }
    R get() {
        if (exc) std::rethrow_exception(exc);
        return *value_ptr;
    }
};

/** job_result specialization for void; there is no value, but exceptions are still captured
 * (rethrown when `get()` is called).
 */
template<>
class job_result<void> {
    std::exception_ptr exc;

public:
    void set_exception(std::exception_ptr e) { exc = std::move(e); }
    // Returns nothing, but rethrows if there is a captured exception.
    void get() { if (exc) std::rethrow_exception(exc); }
};

/// Helper class used to set up batches of jobs to be scheduled via the lokimq job handler.
/// 
/// @tparam R - the return type of the individual jobs
///
template <typename R>
class Batch final : private detail::Batch {
    friend class LokiMQ;
public:
    /// The completion function type, called after all jobs have finished.
    using CompletionFunc = std::function<void(std::vector<job_result<R>> results)>;

    // Default constructor
    Batch() = default;

    // movable
    Batch(Batch&&) = default;
    Batch &operator=(Batch&&) = default;

    // non-copyable
    Batch(const Batch&) = delete;
    Batch &operator=(const Batch&) = delete;

private:
    std::vector<std::function<R()>> jobs;
    std::vector<job_result<R>> results;
    CompletionFunc complete;
    std::size_t jobs_outstanding = 0;
    bool complete_in_proxy = false;
    bool started = false;

    void check_not_started() {
        if (started)
            throw std::logic_error("Cannot add jobs or completion function after starting a lokimq::Batch!");
    }

public:
    /// Preallocates space in the internal vector that stores jobs.
    void reserve(std::size_t num) {
        jobs.reserve(num);
        results.reserve(num);
    }

    /// Adds a job.  This takes any callable object that is invoked with no arguments and returns R
    /// (the Batch return type).  The tasks will be scheduled and run when the next worker thread is
    /// available.  The called function may throw exceptions (which will be propagated to the
    /// completion function through the job_result values).  There is no guarantee on the order of
    /// invocation of the jobs.
    void add_job(std::function<R()> job) {
        check_not_started();
        jobs.emplace_back(std::move(job));
        results.emplace_back();
        jobs_outstanding++;
    }

    /// Sets the completion function to invoke after all jobs have finished.  If this is not set
    /// then jobs simply run and results are discarded.
    void completion(CompletionFunc comp) {
        check_not_started();
        if (complete)
            throw std::logic_error("Completion function can only be set once");
        complete = std::move(comp);
    }

    /// Sets a completion function to invoke *IN THE PROXY THREAD* after all jobs have finished.  Be
    /// very, very careful: this should not be a job that takes any significant amount of CPU time
    /// or can block for any reason (NO MUTEXES).
    void completion_proxy(CompletionFunc comp) {
        check_not_started();
        if (complete)
            throw std::logic_error("Completion function can only be set once");
        complete = std::move(comp);
        complete_in_proxy = true;
    }

private:

    std::size_t size() const override {
        return jobs.size();
    }

    template <typename S = R>
    void set_value(job_result<S>& r, std::function<S()>& f) { r.set_value(f()); }
    void set_value(job_result<void>&, std::function<void()>& f) { f(); }

    void run_job(const int i) override {
        // called by worker thread
        auto& r = results[i];
        try {
            set_value(r, jobs[i]);
        } catch (...) {
            r.set_exception(std::current_exception());
        }
    }

    detail::BatchStatus job_finished() override {
        --jobs_outstanding;
        if (jobs_outstanding)
            return detail::BatchStatus::running;
        if (complete)
            return complete_in_proxy
                ? detail::BatchStatus::complete_proxy
                : detail::BatchStatus::complete;
        return detail::BatchStatus::done;
    }

    void job_completion() override {
        return complete(std::move(results));
    }
};


template <typename R>
void LokiMQ::batch(Batch<R>&& batch) {
    if (batch.size() == 0)
        throw std::logic_error("Cannot batch a a job batch with 0 jobs");
    // Need to send this over to the proxy thread via the base class pointer.  It assumes ownership.
    auto* baseptr = static_cast<detail::Batch*>(new Batch<R>(std::move(batch)));
    detail::send_control(get_control_socket(), "BATCH", bt_serialize(reinterpret_cast<uintptr_t>(baseptr)));
}

}
