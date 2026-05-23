#pragma once

// bench_adapter
//
// Uniform shim that presents a push/pop interface over heterogeneous
// concurrent data structures (MoodyCamel MPMC queue, Intel TBB queue,
// Boost lock-free queue, and signal_tree's signal_set).  This is harness
// scaffolding for the throughput and service-distance benchmarks; it is
// not part of the signal_tree library surface.
//
// Each specialization exposes:
//   - using signal_index   = std::uint64_t;
//   - using selection_hint = std::uint64_t;
//   - explicit bench_adapter(std::size_t capacity);
//   - void          push(signal_index);
//   - signal_index  pop(selection_hint &);
//
// The selection_hint parameter is only used by the signal_tree adapter;
// other backends ignore it.

#include <concurrentqueue.h>
#include <tbb/concurrent_queue.h>
#include <boost/lockfree/queue.hpp>
#include <include/signal_tree.h>

#include <cstddef>
#include <cstdint>


// signal_tree_size ...
// 0 = max speed
// 1 = balance if using hints for priority
// 2,3,4, etc ... large trees, trades throughput for greater control over prioritization if ever needed
static auto constexpr signal_tree_size = 0;


enum class algorithm
{
    moody_camel,
    signal_tree,
    tbb,
    lockfree
};


template <algorithm>
struct bench_adapter;


//==============================================================================
template <>
struct bench_adapter<algorithm::moody_camel>
{
    static auto constexpr name = "MoodyCamel";
    using signal_index   = std::uint64_t;
    using selection_hint = std::uint64_t;

    explicit bench_adapter(std::size_t capacity): queue_(capacity * 2){}

    void push(signal_index signalId)
    {
        while (!queue_.enqueue(signalId))
            ;
    }

    template <typename BeforePublish>
    void push(signal_index signalId, BeforePublish && beforePublish)
    {
        while (true)
        {
            beforePublish();
            if (queue_.enqueue(signalId))
                return;
        }
    }

    signal_index pop(selection_hint &)
    {
        signal_index result;
        while (!queue_.try_dequeue(result))
            ;
        return result;
    }

    moodycamel::ConcurrentQueue<signal_index> queue_;
};


//==============================================================================
template <>
struct bench_adapter<algorithm::tbb>
{
    static auto constexpr name = "TBB";
    using signal_index   = std::uint64_t;
    using selection_hint = std::uint64_t;

    explicit bench_adapter(std::size_t): queue_(){}

    void push(signal_index signalId)
    {
        queue_.push(signalId);
    }

    template <typename BeforePublish>
    void push(signal_index signalId, BeforePublish && beforePublish)
    {
        beforePublish();
        queue_.push(signalId);
    }

    signal_index pop(selection_hint &)
    {
        signal_index result;
        while (!queue_.try_pop(result))
            ;
        return result;
    }

    oneapi::tbb::concurrent_queue<signal_index> queue_;
};


//==============================================================================
template <>
struct bench_adapter<algorithm::lockfree>
{
    static auto constexpr name = "LockFree";
    using signal_index   = std::uint64_t;
    using selection_hint = std::uint64_t;

    explicit bench_adapter(std::size_t capacity)
        : queue_(capacity)
    {
    }

    void push(signal_index signalId)
    {
        while (!queue_.push(signalId))
            ;
    }

    template <typename BeforePublish>
    void push(signal_index signalId, BeforePublish && beforePublish)
    {
        while (true)
        {
            beforePublish();
            if (queue_.push(signalId))
                return;
        }
    }

    signal_index pop(selection_hint &)
    {
        signal_index result;
        while (!queue_.pop(result))
            ;
        return result;
    }

    boost::lockfree::queue<signal_index> queue_;
};


//==============================================================================
// signal_tree adapter presents the same uint64 facade as the queue wrappers;
// signal_id conversion is contained here so the benchmark template stays
// algorithm-agnostic.
template <>
struct bench_adapter<algorithm::signal_tree>
{
    static auto constexpr name = "Signal Tree";
    using signal_set_type = bcpp::signal_set<signal_tree_size>;
    using signal_index    = std::uint64_t;
    using selection_hint  = std::uint64_t;

    explicit bench_adapter(std::size_t capacity)
        : signalSet_(capacity)
    {
    }

    void push(signal_index signalId)
    {
        while (not signalSet_.set(bcpp::signal_id{signalId}))
            ;
    }

    template <typename BeforePublish>
    void push(signal_index signalId, BeforePublish && beforePublish)
    {
        while (true)
        {
            beforePublish();
            if (signalSet_.set(bcpp::signal_id{signalId}))
                return;
        }
    }

    signal_index pop(selection_hint & hint)
    {
        auto h  = bcpp::signal_id{hint};
        auto id = signalSet_.select(h);
        while (not id.valid())
            id = signalSet_.select(h);
        hint = static_cast<std::uint64_t>(h);
        return static_cast<std::uint64_t>(id);
    }

    std::size_t capacity() const noexcept
    {
        return signalSet_.max() + 1ull;
    }

    signal_set_type signalSet_;
};