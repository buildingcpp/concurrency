#pragma once

#include "./process_sharing.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>


namespace bcpp::concurrency::detail
{

    struct no_blocking_state {};


    template <process_sharing sharing>
    class blocking_state;


    struct alignas(64) process_shared_blocking_state
    {
        static_assert(std::atomic<std::int64_t>::is_always_lock_free);
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

        std::atomic<std::int64_t>  nonEmptyTreeCount_{0};
        std::atomic<std::uint32_t> wakeSequence_{0};
        std::atomic<std::uint32_t> stopped_{0};
    };


    template <>
    class blocking_state<process_sharing::process_private>
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


    template <>
    class blocking_state<process_sharing::process_shared>
    {
    public:

        explicit blocking_state(process_shared_blocking_state &) noexcept;

        void increment() noexcept;
        void decrement() noexcept;

        template <typename Clock, typename Duration>
        bool wait_until(std::chrono::time_point<Clock, Duration>);

        void stop() noexcept;
        std::uint64_t count() const noexcept;

    private:

        static void wake(std::atomic<std::uint32_t> &) noexcept;
        static void wait
        (
            std::atomic<std::uint32_t> &,
            std::uint32_t,
            ::timespec const *
        ) noexcept;

        process_shared_blocking_state * state_;
    };

} // namespace bcpp::concurrency::detail


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_private>::increment
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
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_private>::decrement
(
) noexcept
{
    nonEmptyTreeCount_.fetch_sub(1, std::memory_order_release);
}


//==============================================================================
template <typename Clock, typename Duration>
inline bool bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_private>::wait_until
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
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_private>::stop
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
inline std::uint64_t bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_private>::count
(
) const noexcept
{
    auto count = nonEmptyTreeCount_.load(std::memory_order_acquire);
    return (count > 0) ? static_cast<std::uint64_t>(count) : 0;
}


//==============================================================================
inline bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::blocking_state
(
    process_shared_blocking_state & state
) noexcept :
    state_(&state)
{
}


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::increment
(
) noexcept
{
    auto wakeBlockedProcesses = state_->nonEmptyTreeCount_.fetch_add(1, std::memory_order_release) == 0;

    if (wakeBlockedProcesses)
    {
        state_->wakeSequence_.fetch_add(1, std::memory_order_release);
        wake(state_->wakeSequence_);
    }
}


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::decrement
(
) noexcept
{
    state_->nonEmptyTreeCount_.fetch_sub(1, std::memory_order_release);
}


//==============================================================================
template <typename Clock, typename Duration>
inline bool bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::wait_until
(
    std::chrono::time_point<Clock, Duration> deadline
)
{
    for (;;)
    {
        if (state_->stopped_.load(std::memory_order_acquire) != 0)
            return false;

        if (state_->nonEmptyTreeCount_.load(std::memory_order_acquire) > 0)
            return true;

        auto const sequence = state_->wakeSequence_.load(std::memory_order_acquire);

        if (state_->stopped_.load(std::memory_order_acquire) != 0)
            return false;

        if (state_->nonEmptyTreeCount_.load(std::memory_order_acquire) > 0)
            return true;

        auto const now = Clock::now();
        if (now >= deadline)
            return false;

        auto const remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
        auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
        auto const nanoseconds = remaining - seconds;
        ::timespec timeout
        {
            static_cast<::time_t>(seconds.count()),
            static_cast<long>(nanoseconds.count())
        };

        wait(state_->wakeSequence_, sequence, &timeout);
    }
}


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::stop
(
) noexcept
{
    state_->stopped_.store(1, std::memory_order_release);
    state_->wakeSequence_.fetch_add(1, std::memory_order_release);
    wake(state_->wakeSequence_);
}


//==============================================================================
inline std::uint64_t bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::count
(
) const noexcept
{
    auto count = state_->nonEmptyTreeCount_.load(std::memory_order_acquire);
    return (count > 0) ? static_cast<std::uint64_t>(count) : 0;
}


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::wake
(
    std::atomic<std::uint32_t> & sequence
) noexcept
{
    auto * address = reinterpret_cast<std::uint32_t *>(&sequence);
    static_cast<void>(::syscall(SYS_futex, address, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
}


//==============================================================================
inline void bcpp::concurrency::detail::blocking_state<
        bcpp::concurrency::process_sharing::process_shared>::wait
(
    std::atomic<std::uint32_t> & sequence,
    std::uint32_t expected,
    ::timespec const * timeout
) noexcept
{
    auto * address = reinterpret_cast<std::uint32_t *>(&sequence);
    static_cast<void>(::syscall(SYS_futex, address, FUTEX_WAIT, expected, timeout, nullptr, 0));
}
