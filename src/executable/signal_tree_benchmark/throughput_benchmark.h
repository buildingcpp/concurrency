#pragma once

#include "./benchmark_affinity.h"

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
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>


namespace throughput_benchmark
{

    //==============================================================================
    static constexpr auto active_signal_count = std::size_t{1 << 16};
    static constexpr auto sparsity_factor     = std::size_t{4};
    static constexpr auto capacity            = active_signal_count * sparsity_factor;

    inline constexpr auto ansi_cv_bad   = "\033[1;30;103m";
    inline constexpr auto ansi_reset    = "\033[0m";

    inline constexpr auto cv_bad_threshold  = 1.00L;


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
    inline auto make_active_signals()
    {
        using signal_index = std::uint64_t;

        std::vector<signal_index> activeSignals;
        activeSignals.reserve(active_signal_count);

        for (auto i = 0ull; i < active_signal_count; ++i)
        {
            auto bucket = i * sparsity_factor;
            auto offset = (i * 17ull) % sparsity_factor;
            activeSignals.push_back(signal_index{bucket + offset});
        }

        return activeSignals;
    }


    //==============================================================================
    inline auto static_gather_stats(auto input)
        -> std::tuple<std::uint64_t, long double, long double, long double>
    {
        auto total = std::uint64_t{0};
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
    inline std::string cv_text
    (
        long double value
    )
    {
        return fmt::format("{:.4f}", static_cast<double>(value));
    }


    //==============================================================================
    inline std::string cv_column
    (
        long double value,
        std::size_t width
    )
    {
        auto text = cv_text(value);
        auto padding = (width > text.size()) ? std::string(width - text.size(), ' ') : std::string{};
        if (value >= cv_bad_threshold)
            return fmt::format("{}{}{}{}", ansi_cv_bad, text, ansi_reset, padding);
        return text + padding;
    }


    //==============================================================================
    inline void print_throughput_configuration_once
    (
        std::chrono::nanoseconds testDuration,
        std::size_t numThreads
    )
    {
        static auto printed = false;
        if (printed)
            return;
        printed = true;
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(testDuration).count();
        benchmark_affinity::print_environment_once(cores, numThreads);

        std::cout << "==================================================================================\n";
        std::cout << "Benchmark: Throughput\n";
        std::cout << "active_signal_count: " << active_signal_count << "\n";
        std::cout << "capacity: " << capacity << "\n";
        std::cout << "sparsity_factor: " << sparsity_factor << "\n";
        std::cout << "duration_ms: " << durationMs << "\n";
        std::cout << "legend:\n";
        std::cout << "  " << ansi_cv_bad << "  " << ansi_reset
                  << " coefficient of variation >= " << static_cast<double>(cv_bad_threshold)
                  << " (standard deviation >= mean)\n";
    }


    //==============================================================================
    inline void static_print_stats
    (
        std::size_t numThreads,
        std::chrono::nanoseconds duration,
        auto const & signalStats,
        auto const & threadStats
    )
    {
        auto [signalTotal, signalMean, signalSd, signalCv] = static_gather_stats(std::span(signalStats.data(), signalStats.size()));
        auto [threadTotal, threadMean, threadSd, threadCv] = static_gather_stats(std::span(threadStats.data(), threadStats.size()));
        auto seconds = static_cast<double>(duration.count()) / 1'000'000'000.0;
        auto signalsPerSecond = static_cast<double>(signalTotal) / seconds;
        auto signalsPerThreadSecond = signalsPerSecond / static_cast<double>(numThreads);
        auto signalCvColumn = cv_column(signalCv, 22);
        auto threadCvColumn = cv_column(threadCv, 18);

        std::cout << fmt::format
        (
            "{:<15}{:<20.0f}{:<25.0f}{}{}\n",
            numThreads,
            signalsPerSecond,
            signalsPerThreadSecond,
            signalCvColumn,
            threadCvColumn
        );
    }


    //==============================================================================
    template <algorithm Algorithm>
    void run_benchmark
    (
        std::size_t numThreads,
        std::chrono::nanoseconds testDuration
    )
    {
        using container_type = bench_adapter<Algorithm>;
        using signal_index   = typename container_type::signal_index;

        auto activeSignals = make_active_signals();
        auto queue = std::make_unique<container_type>(capacity);
        for (auto signal : activeSignals)
            queue->push(signal_index{signal});
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
            thread = std::jthread
            (
                [&, threadId = index++](auto)
                {
                    benchmark_affinity::set_cpu_affinity(cores[threadId]);
                    std::vector<std::uint64_t> localSelectedCount(capacity, 0);
                    std::uint64_t localThreadSelectionCount = 0;

                    ++activeThreadCount;
                    while (not beginTest)
                        ;

                    signal_index selectionHint(threadId * (capacity / numThreads));
                    while (not endTest)
                    {
                        auto x = queue->pop(selectionHint);
                        queue->push(signal_index{static_cast<std::uint64_t>(x)});
                        ++localSelectedCount[static_cast<std::size_t>(x)];
                        ++localThreadSelectionCount;
                    }

                    for (auto signal : activeSignals)
                    {
                        auto i = static_cast<std::size_t>(signal);
                        if (localSelectedCount[i] != 0)
                            signalSelectedCount[i] += localSelectedCount[i];
                    }

                    threadSelectionCount[threadId] = localThreadSelectionCount;
                    --activeThreadCount;
                }
            );
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
        selectionCount.reserve(activeSignals.size());
        for (auto signal : activeSignals)
            selectionCount.push_back(signalSelectedCount[static_cast<std::size_t>(signal)]);

        static_print_stats(numThreads, elapsed, selectionCount, threadSelectionCount);
    }


    //==============================================================================
    template <algorithm Algorithm>
    void benchmark
    (
        std::size_t numThreads,
        std::chrono::nanoseconds testDuration
    )
    {
        print_throughput_configuration_once(testDuration, numThreads);

        std::cout << "==================================================================================\n";
        std::cout << "Algorithm: " << algorithm_name<Algorithm>() << "\n";

        std::cout << fmt::format
        (
            "{:<15}{:<20}{:<25}{:<22}{:<18}\n",
            "Thread_Count:",
            "Signals_per_Second:",
            "Signals_per_Thread/sec:",
            "Signal_Selection_cv:",
            "Thread_Work_cv:"
        );

        for (auto i = 1ull; i <= numThreads; ++i)
            run_benchmark<Algorithm>(i, testDuration);
    }

} // namespace throughput_benchmark
