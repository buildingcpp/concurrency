#pragma once

#include "./benchmark_affinity.h"
#include "./signal_placement.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>


namespace adversarial_throughput_benchmark
{

    //==============================================================================
    //
    // Adversarial throughput benchmark.
    //
    // One shared queue under test.  K active signals are present at any instant,
    // but the scheduled id space is K * cycle_length.  Each logical signal is a
    // disjoint cycle of cycle_length physical ids, and every pop migrates the set
    // bit to the next physical id in that cycle.
    //
    // Cycles are strided across capacity:
    //
    //     cycle c = { c, c + K, c + 2K, ... }
    //
    // So a push usually lands far away from the popped id.  For Signal Tree this
    // intentionally creates cross-subtree movement instead of the static locality
    // used by the ordinary throughput benchmark.
    //
    // Hot loop per thread:
    //
    //     X = queue.pop()
    //     queue.push(next[X])
    //     ++count[X]
    //
    // Because each cycle holds exactly one set bit, next[X] is guaranteed clear
    // when X is popped.  The benchmark therefore needs no per-thread parking pool
    // and no second synchronization object.
    //
    // Reported numbers:
    //     - total signals/sec
    //     - signals/thread/sec
    //     - CV across all scheduled physical signal ids
    //     - CV across per-thread total operation counts
    //
    //==============================================================================

    static constexpr auto active_signal_count    = std::size_t{1 << 14};
    static constexpr auto cycle_length           = std::size_t{7};
    static constexpr auto scheduled_signal_count = active_signal_count * cycle_length;

    using placement_type = signal_placement<scheduled_signal_count>;

    static constexpr auto capacity = placement_type::physical_capacity;

    inline constexpr auto ansi_cv_bad        = "\033[1;30;103m";
    inline constexpr auto ansi_reset         = "\033[0m";
    inline constexpr auto cv_bad_threshold   = 1.00L;


    //==============================================================================
    template <algorithm Algorithm>
    inline std::string algorithm_name()
    {
        if constexpr (Algorithm == algorithm::signal_tree)
            return fmt::format("{}<{}>", bench_adapter<Algorithm>::name, signal_tree_size);
        else
            return std::string{bench_adapter<Algorithm>::name};
    }


    //==============================================================================
    inline auto gather_stats(auto const & input)
        -> std::tuple<std::uint64_t, long double, long double, long double>
    {
        auto total = 0ull;
        for (auto value : input)
            total += static_cast<std::uint64_t>(value);
        auto mean = static_cast<long double>(total) / static_cast<long double>(input.size());
        auto variance = 0.0L;
        for (auto value : input)
        {
            auto delta = static_cast<long double>(value) - mean;
            variance += delta * delta;
        }
        if (input.size() > 1)
            variance /= static_cast<long double>(input.size() - 1);
        auto sd = std::sqrt(variance);
        auto cv = (mean == 0.0L) ? 0.0L : sd / mean;
        return {total, mean, sd, cv};
    }


    //==============================================================================
    inline std::string cv_text(long double value)
    {
        return fmt::format("{:.4f}", static_cast<double>(value));
    }


    //==============================================================================
    inline std::string cv_column(long double value, std::size_t width)
    {
        auto text = cv_text(value);
        auto padding = (width > text.size()) ? std::string(width - text.size(), ' ') : std::string{};
        if (value >= cv_bad_threshold)
            return fmt::format("{}{}{}{}", ansi_cv_bad, text, ansi_reset, padding);
        return text + padding;
    }


    //==============================================================================
    inline void print_configuration_once(std::chrono::nanoseconds testDuration, std::size_t numThreads)
    {
        static auto printed = false;
        if (printed)
            return;
        printed = true;
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(testDuration).count();

        benchmark_affinity::print_environment_once(cores, numThreads);

        std::cout << "==================================================================================\n";
        std::cout << "Benchmark: Adversarial Throughput\n";
        std::cout << "active_signal_count: " << active_signal_count << "\n";
        std::cout << "scheduled_signal_count: " << scheduled_signal_count << "\n";
        std::cout << "physical_capacity: " << capacity << "\n";
        std::cout << "cycle_length: " << cycle_length << "\n";
        std::cout << "placement: densest Signal Tree<" << placement_type::placement_tree_size << ">\n";
        std::cout << "duration_ms: " << durationMs << "\n";
        std::cout << "workload: adversarial one-hot cycles; pop X, push placed next[X]\n";
        std::cout << "legend:\n";
        std::cout << "  " << ansi_cv_bad << "  " << ansi_reset
                  << " coefficient of variation >= " << static_cast<double>(cv_bad_threshold)
                  << " (standard deviation >= mean)\n";
    }


    //==============================================================================
    inline void print_stats
    (
        std::size_t numThreads,
        std::chrono::nanoseconds duration,
        auto const & signalStats,
        auto const & threadStats
    )
    {
        auto [signalTotal, signalMean, signalSd, signalCv] =
            gather_stats(std::span(signalStats.data(), signalStats.size()));
        auto [threadTotal, threadMean, threadSd, threadCv] =
            gather_stats(std::span(threadStats.data(), threadStats.size()));
        auto seconds = static_cast<double>(duration.count()) / 1'000'000'000.0;
        auto signalsPerSecond = static_cast<double>(signalTotal) / seconds;
        auto signalsPerThreadSecond = signalsPerSecond / static_cast<double>(numThreads);
        auto signalCvColumn = cv_column(signalCv, 22);
        auto threadCvColumn = cv_column(threadCv, 18);

        std::cout << fmt::format(
            "{:<15}{:<20.0f}{:<25.0f}{}{}\n",
            numThreads,
            signalsPerSecond,
            signalsPerThreadSecond,
            signalCvColumn,
            threadCvColumn);
    }


    //==============================================================================
    template <algorithm Algorithm>
    void run_benchmark(std::size_t numThreads, std::chrono::nanoseconds testDuration)
    {
        using container_type = bench_adapter<Algorithm>;
        using signal_index   = typename container_type::signal_index;

        placement_type placement;

        std::vector<signal_index> next(capacity);
        for (auto c = 0ull; c < active_signal_count; ++c)
        {
            for (auto i = 0ull; i < cycle_length; ++i)
            {
                auto cur = c + i * active_signal_count;
                auto nxt = c + ((i + 1) % cycle_length) * active_signal_count;
                next[static_cast<std::size_t>(placement.physical(cur))] = signal_index{placement.physical(nxt)};
            }
        }

        auto queue = std::make_unique<container_type>(capacity);

        for (auto c = 0ull; c < active_signal_count; ++c)
        {
            auto startIndex = c % cycle_length;
            auto startPosition = c + startIndex * active_signal_count;
            queue->push(signal_index{placement.physical(startPosition)});
        }

        std::atomic<bool> beginTest{false};
        std::atomic<bool> endTest{false};
        std::atomic<std::size_t> activeThreadCount{0};
        std::vector<std::jthread> threads(numThreads);

        std::vector<std::atomic<std::uint64_t>> signalSelectedCount(capacity);
        for (auto & c : signalSelectedCount)
            c = 0;
        std::vector<std::uint64_t> threadSelectionCount(numThreads, 0);

        for (auto index = 0ull; auto & thread : threads)
        {
            thread = std::jthread(
                [&, threadId = index++](auto)
                {
                    benchmark_affinity::set_cpu_affinity(cores[threadId]);
                    std::vector<std::uint64_t> localSelectedCount(capacity, 0);
                    auto localThreadSelectionCount = std::uint64_t{0};
                    ++activeThreadCount;
                    while (not beginTest)
                        ;
                    signal_index selectionHint(threadId * (capacity / numThreads));
                    while (not endTest)
                    {
                        auto x = queue->pop(selectionHint);
                        queue->push(next[static_cast<std::size_t>(x)]);
                        ++localSelectedCount[static_cast<std::size_t>(x)];
                        ++localThreadSelectionCount;
                    }

                    for (auto i = 0ull; i < capacity; ++i)
                    {
                        if (localSelectedCount[i] != 0)
                            signalSelectedCount[i] += localSelectedCount[i];
                    }
                    threadSelectionCount[threadId] = localThreadSelectionCount;
                    --activeThreadCount;
                });
        }

        while (activeThreadCount < numThreads)
            ;

        auto start = std::chrono::steady_clock::now();
        beginTest = true;
        std::this_thread::sleep_for(testDuration);
        endTest = true;
        while (activeThreadCount > 0)
            ;
        auto finish = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start);

        for (auto & thread : threads)
            thread.join();

        std::vector<std::uint64_t> selectionCount;
        selectionCount.reserve(scheduled_signal_count);

        for (auto logical = 0ull; logical < scheduled_signal_count; ++logical)
        {
            auto physical = static_cast<std::size_t>(placement.physical(logical));
            selectionCount.push_back(signalSelectedCount[physical]);
        }
        print_stats(numThreads, elapsed, selectionCount, threadSelectionCount);
    }


    //==============================================================================
    template <algorithm Algorithm>
    void benchmark(std::size_t numThreads, std::chrono::nanoseconds testDuration)
    {
        print_configuration_once(testDuration, numThreads);

        std::cout << "==================================================================================\n";
        std::cout << "Algorithm: " << algorithm_name<Algorithm>() << "\n";

        std::cout << fmt::format(
            "{:<15}{:<20}{:<25}{:<22}{:<18}\n",
            "Thread_Count:",
            "Signals_per_Second:",
            "Signals_per_Thread/sec:",
            "Signal_Selection_cv:",
            "Thread_Work_cv:");

        for (auto i = 1ull; i <= numThreads; ++i)
            run_benchmark<Algorithm>(i, testDuration);
    }

} // namespace adversarial_throughput_benchmark
