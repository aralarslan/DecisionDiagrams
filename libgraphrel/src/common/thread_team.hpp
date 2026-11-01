// Private header — not installed.

#ifndef GRAPHREL_SRC_COMMON_THREAD_TEAM_HPP
#define GRAPHREL_SRC_COMMON_THREAD_TEAM_HPP

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace teddy::graphrel::detail
{

/**
 * @brief A fixed group of threads that run one job at a time, together.
 *
 * run(job) calls job(t) for t = 0..size()-1, job(0) on the calling thread,
 * and returns when every call has returned. The first exception thrown by
 * any call is rethrown by run(), after all calls have finished (they may
 * refer to the caller's stack).
 */
class thread_team
{
public:
    explicit thread_team(unsigned size) : size_(size == 0 ? 1 : size)
    {
        try
        {
            for (unsigned t = 1; t < size_; ++t)
                workers_.emplace_back([this, t] { work(t); });
        }
        catch (...)
        {
            stop();
            throw;
        }
    }

    ~thread_team()
    {
        stop();
    }

    thread_team(thread_team const&)             = delete;
    thread_team& operator= (thread_team const&) = delete;

    unsigned size () const
    {
        return size_;
    }

    void run (std::function<void(unsigned)> const& job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_     = &job;
            pending_ = workers_.size();
            error_   = nullptr;
            ++generation_;
        }
        start_.notify_all();
        std::exception_ptr own;
        try
        {
            job(0);
        }
        catch (...)
        {
            own = std::current_exception();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
        job_ = nullptr;
        if (own)
            std::rethrow_exception(own);
        if (error_)
            std::rethrow_exception(error_);
    }

private:
    void work (unsigned t)
    {
        std::size_t seen = 0;
        for (;;)
        {
            std::function<void(unsigned)> const* job = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                start_.wait(
                    lock,
                    [&] { return stopping_ || generation_ != seen; }
                );
                if (stopping_)
                    return;
                seen = generation_;
                job  = job_;
            }
            std::exception_ptr error;
            try
            {
                (*job)(t);
            }
            catch (...)
            {
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (error && ! error_)
                    error_ = error;
                --pending_;
            }
            done_.notify_one();
        }
    }

    void stop ()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        start_.notify_all();
        for (std::thread& w : workers_)
            w.join();
        workers_.clear();
    }

    unsigned size_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable start_, done_;
    std::function<void(unsigned)> const* job_ = nullptr;
    std::size_t pending_                      = 0;
    std::size_t generation_                   = 0;
    bool stopping_                            = false;
    std::exception_ptr error_;
};

} // namespace teddy::graphrel::detail

#endif // GRAPHREL_SRC_COMMON_THREAD_TEAM_HPP
