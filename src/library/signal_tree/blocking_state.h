#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>


namespace bcpp::concurrency::detail
{

    struct no_blocking_state {};


    class blocking_state
    {
    public:

        void increment() noexcept;
        void decrement() noexcept;

        template <typename Clock, typename Duration>
        bool wait_until(std::chrono::time_point<Clock, Duration>);

        void stop() noexcept;
        std::uint64_t count() const noexcept;

    private:

        std::atomic<std::int64_t>  nonEmptyTreeCount_{0};
        std::mutex                 mutex_;
        std::condition_variable    conditionVariable_;
        bool                       stopped_{false};
    };

} // namespace bcpp::concurrency::detail


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state::increment
(
) noexcept
{
    auto wakeBlockedThreads = nonEmptyTreeCount_.fetch_add(1, std::memory_order_release) == 0;

    if (wakeBlockedThreads)
    {
        std::lock_guard lock(mutex_);
        conditionVariable_.notify_all();
    }
}


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state::decrement
(
) noexcept
{
    nonEmptyTreeCount_.fetch_sub(1, std::memory_order_release);
}


//==============================================================================
template <typename Clock, typename Duration>
inline bool bcpp::concurrency::detail::blocking_state::wait_until
(
    std::chrono::time_point<Clock, Duration> deadline
)
{
    std::unique_lock lock(mutex_);

    if (stopped_)
        return false;

    if (not conditionVariable_.wait_until(lock, deadline, [this]
        {
            return (nonEmptyTreeCount_.load(std::memory_order_acquire) > 0) || stopped_;
        }))
        return false;

    return not stopped_;
}


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state::stop
(
) noexcept
{
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    conditionVariable_.notify_all();
}


//==============================================================================
inline std::uint64_t bcpp::concurrency::detail::blocking_state::count
(
) const noexcept
{
    auto count = nonEmptyTreeCount_.load(std::memory_order_acquire);
    return (count > 0) ? static_cast<std::uint64_t>(count) : 0;
}
