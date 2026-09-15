#pragma once

#include "./benchmark_affinity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


namespace service_distance_benchmark
{
    // Benchmark configuration.
    // target_num_signals is the target number of signals active in the shared
    // queue at steady state.  The actual active count is rounded down to an
    // even per-thread partition for thread counts that do not divide it.
    //
    // K ids are active, K paired ids are clear, and the remaining address
    // space is unused sparse slack.
    inline constexpr auto target_num_signals = std::size_t{1 << 14};
    inline constexpr auto id_space_factor    = std::size_t{4};
    static_assert(id_space_factor >= 2);

    //==============================================================================
    template <typename T>
    T percentile_sorted_nearest_rank(const std::vector<T> & sorted, double p)
    {
        if (sorted.empty())
            throw std::invalid_argument("percentile of empty sample set");

        if (p <= 0.0)
            return sorted.front();

        if (p >= 100.0)
            return sorted.back();

        auto n = sorted.size();
        auto idx = static_cast<std::size_t>((p / 100.0) * static_cast<double>(n));
        auto exact = (p / 100.0) * static_cast<double>(n);
        if (static_cast<double>(idx) < exact)
            ++idx;

        if (idx == 0)
            idx = 1;
        --idx;
        if (idx >= n)
            idx = n - 1;
        return sorted[idx];
    }


    //==============================================================================
    inline std::vector<std::size_t> shuffled_indices
    (
        std::size_t count,
        std::uint64_t seed
    )
    {
        std::vector<std::size_t> result(count);
        std::iota(result.begin(), result.end(), std::size_t{0});

        std::mt19937_64 rng(seed);
        std::shuffle(result.begin(), result.end(), rng);

        return result;
    }


    //==============================================================================
    struct coverage_stats
    {
        std::size_t participatingIds_{};
        std::size_t selectedIds_{};
        std::size_t sampledIds_{};
        std::size_t coverageFailureIds_{};
        std::uint64_t minSelectedCount_{};
        std::uint64_t maxSelectedCount_{};
        double selectedCountCv_{};
    };


    //==============================================================================
    inline coverage_stats make_coverage_stats
    (
        std::vector<std::size_t> const & participatingIds,
        std::vector<std::uint64_t> const & selectedCounts,
        std::vector<std::uint64_t> const & sampledCounts
    )
    {
        coverage_stats result;
        result.participatingIds_ = participatingIds.size();
        result.minSelectedCount_ = std::numeric_limits<std::uint64_t>::max();

        auto totalSelected = 0.0L;

        for (auto id : participatingIds)
        {
            auto selected = selectedCounts[id];
            auto sampled  = sampledCounts[id];

            if (selected != 0)
                ++result.selectedIds_;

            if (sampled != 0)
                ++result.sampledIds_;

            result.minSelectedCount_ = std::min(result.minSelectedCount_, selected);
            result.maxSelectedCount_ = std::max(result.maxSelectedCount_, selected);
            totalSelected += static_cast<long double>(selected);
        }

        result.coverageFailureIds_ = result.participatingIds_ - result.sampledIds_;
        if (result.participatingIds_ == 0)
        {
            result.minSelectedCount_ = 0;
            result.selectedCountCv_ = 0.0;
            return result;
        }

        auto mean = totalSelected / static_cast<long double>(result.participatingIds_);
        auto variance = 0.0L;
        for (auto id : participatingIds)
        {
            auto delta = static_cast<long double>(selectedCounts[id]) - mean;
            variance += delta * delta;
        }

        variance /= static_cast<long double>(result.participatingIds_);
        auto sd = std::sqrt(variance);
        result.selectedCountCv_ = (mean == 0.0L) ? 0.0 : static_cast<double>(sd / mean);
        return result;
    }


    inline constexpr auto ansi_cv_bad      = "\033[1;30;103m";
    inline constexpr auto ansi_coverage_failure = "\033[1;37;41m";
    inline constexpr auto ansi_reset       = "\033[0m";

    inline constexpr auto cv_bad_threshold  = 1.00;


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
    inline std::string format_anomalous_count
    (
        std::size_t value,
        std::size_t total,
        std::size_t width,
        char const * ansi
    )
    {
        auto text = (value == 0 || total == 0)
            ? fmt::format("{}", value)
            : fmt::format("{} [{:.1f}%]", value, 100.0 * static_cast<double>(value) / static_cast<double>(total));
        auto padding = std::string((text.size() < width) ? width - text.size() : 0, ' ');
        if (value == 0)
            return text + padding;
        return fmt::format("{}{}{}{}", ansi, text, ansi_reset, padding);
    }


    //==============================================================================
    inline std::string format_decimal_text
    (
        double value
    )
    {
        return fmt::format("{:.3f}", value);
    }


    //==============================================================================
    inline std::string format_decimal
    (
        double value,
        std::size_t width
    )
    {
        auto text = format_decimal_text(value);
        auto padding = std::string((text.size() < width) ? width - text.size() : 0, ' ');
        return text + padding;
    }


    //==============================================================================
    inline std::string format_cv
    (
        double value,
        std::size_t width
    )
    {
        auto text = fmt::format("{:.6f}", value);
        auto padding = std::string((text.size() < width) ? width - text.size() : 0, ' ');
        if (value >= cv_bad_threshold)
            return fmt::format("{}{}{}{}", ansi_cv_bad, text, ansi_reset, padding);
        return text + padding;
    }


    //==============================================================================
    inline void print_service_distribution_configuration_once
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

        std::cout << "==================================================================================\n";
        std::cout << "Benchmark: Service Distance\n";
        std::cout << "workload: shared queue, deterministic two-signal cycles\n";
        std::cout << "id_layout: fixed-seed shuffled 2-id cycles across "
                  << id_space_factor << "x sparse id space\n";
        std::cout << "coverage: measured over scheduled 2-cycle ids only\n";
        std::cout << "target_num_signals: " << target_num_signals << "\n";
        std::cout << "duration_ms: " << durationMs << "\n";
        std::cout << "legend:\n";
        std::cout << "  " << ansi_cv_bad << "  " << ansi_reset
                  << " coefficient of variation >= " << cv_bad_threshold
                  << " (standard deviation >= mean)\n";
        std::cout << "  " << ansi_coverage_failure << "  " << ansi_reset
                  << " coverage failure: scheduled signals which were never serviced\n";
    }


    //==============================================================================
    inline void print_service_distribution_header
    (
        std::ostream& os = std::cout
    )
    {
        os << fmt::format
        (
            "{:<8}{:<12}{:<10}{:<8}{:<8}{:<8}{:<8}{:<8}{:<18}{:<12}{:<12}{:<9}\n",
            "threads",
            "samples",
            "active",
            "p50_x",
            "p90_x",
            "p99_x",
            "p99.9",
            "max_x",
            "coverage failure",
            "min_sel",
            "max_sel",
            "sel_cv"
        );
    }


    //==============================================================================
    template <typename T>
    void print_service_distribution_summary
    (
        std::size_t threadCount,
        std::vector<T> const & sortedSamples,
        std::size_t activeSignalCount,
        coverage_stats const & coverage,
        std::ostream & os = std::cout
    )
    {
        if (sortedSamples.empty())
            throw std::invalid_argument("empty sample set");

        auto norm = [activeSignalCount](auto v)
        {
            return static_cast<double>(v) / static_cast<double>(activeSignalCount);
        };

        auto p50  = norm(percentile_sorted_nearest_rank(sortedSamples, 50.0));
        auto p90  = norm(percentile_sorted_nearest_rank(sortedSamples, 90.0));
        auto p99  = norm(percentile_sorted_nearest_rank(sortedSamples, 99.0));
        auto p999 = norm(percentile_sorted_nearest_rank(sortedSamples, 99.9));
        auto max  = norm(sortedSamples.back());

        auto p50Text  = format_decimal(p50, 8);
        auto p90Text  = format_decimal(p90, 8);
        auto p99Text  = format_decimal(p99, 8);
        auto p999Text = format_decimal(p999, 8);
        auto maxText  = format_decimal(max, 8);

        auto coverageFailureText = format_anomalous_count(coverage.coverageFailureIds_, coverage.participatingIds_, 18, ansi_coverage_failure);
        auto selectedCvText = format_cv(coverage.selectedCountCv_, 9);

        os << fmt::format
        (
            "{:<8}{:<12}{:<10}{}{}{}{}{}{}{:<12}{:<12}{}\n",
            threadCount,
            sortedSamples.size(),
            activeSignalCount,
            p50Text,
            p90Text,
            p99Text,
            p999Text,
            maxText,
            coverageFailureText,
            coverage.minSelectedCount_,
            coverage.maxSelectedCount_,
            selectedCvText
        );
    }


    //==============================================================================
    //
    // Service-distance benchmark.
    //
    // One shared queue under test.  All threads compete on it.  The queue holds
    // exactly K active signals at steady state.
    //
    // This version does not use per-thread parked pools.  Instead, the benchmark
    // builds K disjoint two-signal cycles.  Exactly one signal in each pair is
    // active at any moment.  When a worker pops x, it pushes next[x], which is
    // guaranteed clear by construction.
    //
    // Hot loop:
    //     x = queue.pop(hint)           // x becomes clear
    //     y = next[x]                   // y is known clear
    //     queue.push(y, before_publish) // timestamp y immediately before publication
    //                                   // y becomes active
    //
    // Service distance is measured as queue residence: timestamp at publication,
    // sample at the next pop of that same id.  The global epoch increments once
    // per pop across all threads, so a FIFO queue with K active signals should
    // converge to K epochs of residence, i.e. 1.0x after normalization.
    //
    // Coverage accounting is reported over the scheduled population only: the 2K
    // ids participating in the K two-signal cycles.  Unused sparse address-space
    // slack is intentionally excluded.  This catches algorithms that produce
    // attractive service-distance percentiles over a subset while completely
    // failing to service other scheduled ids.
    //
    // ID layout:
    //   The id space is sparse relative to K.  The benchmark uses a fixed-seed
    //   deterministic shuffle of the whole id space, then builds K shuffled pairs.
    //   The first id in each pair is initially active; the second id is initially
    //   clear.  The read-only next[] map alternates each pair forever.
    //
    //==============================================================================
    template <algorithm Algorithm>
    void run_benchmark
    (
        std::size_t numThreads,
        std::chrono::nanoseconds testDuration
    )
    {
        using container_type = bench_adapter<Algorithm>;
        using selection_hint = typename container_type::selection_hint;
        using signal_index   = typename container_type::signal_index;

        // Target K = signals active in the shared queue at any moment.
        auto activeSignalCount = target_num_signals;

        // Only 2K ids participate in this benchmark; the remaining ids are
        // unused address-space slack.
        auto capacity = id_space_factor * activeSignalCount;

        // Fixed deterministic layout: random-looking but reproducible.
        // Each adjacent pair in this shuffled list becomes one two-signal cycle.
        static constexpr auto layout_seed = std::uint64_t{0x9e3779b97f4a7c15ull};
        auto idOrder = shuffled_indices(capacity, layout_seed);

        std::vector<signal_index> nextSignal(capacity, signal_index{});
        std::vector<std::size_t> participatingIds;
        participatingIds.reserve(2 * activeSignalCount);

        auto queue = std::make_unique<container_type>(capacity);

        for (auto i = 0ull; i < activeSignalCount; ++i)
        {
            auto activeId = signal_index{idOrder[2 * i]};
            auto clearId  = signal_index{idOrder[2 * i + 1]};
            nextSignal[static_cast<std::size_t>(activeId)] = clearId;
            nextSignal[static_cast<std::size_t>(clearId)]  = activeId;
            participatingIds.push_back(static_cast<std::size_t>(activeId));
            participatingIds.push_back(static_cast<std::size_t>(clearId));
            queue->push(activeId);
        }

        // Shared per-signal timestamps.  Set at push, read at pop.
        std::vector<std::atomic<std::uint64_t>> signalTimestamp(capacity);
        for (auto & ts : signalTimestamp)
            ts = 0;

        // Shared global epoch.  Incremented once per pop across all threads.
        std::atomic<std::uint64_t> globalEpoch{0};

        std::atomic<bool> beginTest{false};
        std::atomic<bool> endTest{false};
        std::atomic<std::size_t> activeThreadCount{0};

        std::vector<std::jthread> threads(numThreads);
        std::vector<std::vector<std::uint64_t>> threadSamples(numThreads);
        std::vector<std::vector<std::uint64_t>> threadSelectedCounts(numThreads);
        std::vector<std::vector<std::uint64_t>> threadSampledCounts(numThreads);

        for (auto index = std::size_t{0}; auto & thread : threads)
        {
            thread = std::jthread
            (
                [&, threadId = index++](auto)
                {
                    benchmark_affinity::set_cpu_affinity(cores[threadId]);

                    auto constexpr max_sample_count = std::size_t{1 << 20};
                    std::vector<std::uint64_t> localSample;
                    localSample.reserve(max_sample_count);

                    std::vector<std::uint64_t> localSelectedCount(capacity, 0);
                    std::vector<std::uint64_t> localSampledCount(capacity, 0);
                    ++activeThreadCount;

                    while (not beginTest)
                        ;

                    selection_hint selectionHint(threadId * (capacity / numThreads));

                    while (not endTest)
                    {
                        auto x = queue->pop(selectionHint);
                        auto xIndex = static_cast<std::size_t>(x);
                        ++localSelectedCount[xIndex];

                        auto epoch = globalEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
                        auto prev = signalTimestamp[xIndex].load(std::memory_order_relaxed);

                        // Skip the first selection of each signal: timestamp 0
                        // means the id was part of initial seeding rather than
                        // a measured push event.
                        if (prev != 0)
                        {
                            ++localSampledCount[xIndex];
                            if (localSample.size() < localSample.capacity())
                                localSample.push_back(epoch - prev);
                        }

                        auto y = nextSignal[xIndex];
                        auto yIndex = static_cast<std::size_t>(y);

                        // y is paired with x and is known clear because exactly
                        // one signal in each pair is active at any moment.  Stamp
                        // it immediately before publication.  This keeps queue-
                        // residence measurement from including backend push
                        // spinning/retry time while y is still invisible.
                        queue->push
                        (
                            y, [&]{signalTimestamp[yIndex].store(globalEpoch.load(std::memory_order_relaxed),std::memory_order_relaxed);}
                        );
                    }

                    threadSamples[threadId] = std::move(localSample);
                    threadSelectedCounts[threadId] = std::move(localSelectedCount);
                    threadSampledCounts[threadId] = std::move(localSampledCount);
                    --activeThreadCount;
                }
            );
        }

        while (activeThreadCount < numThreads)
            ;

        beginTest = true;
        std::this_thread::sleep_for(testDuration);
        endTest = true;

        while (activeThreadCount > 0)
            ;

        for (auto & thread : threads)
            thread.join();

        // Aggregate per-thread samples.
        std::vector<std::uint64_t> globalSamples;
        std::size_t totalSize = 0;
        for (auto & s : threadSamples)
            totalSize += s.size();
        globalSamples.reserve(totalSize);
        for (auto & s : threadSamples)
            std::move(s.begin(), s.end(), std::back_inserter(globalSamples));

        std::sort(globalSamples.begin(), globalSamples.end());

        // Aggregate per-id coverage counts.
        std::vector<std::uint64_t> selectedCounts(capacity, 0);
        std::vector<std::uint64_t> sampledCounts(capacity, 0);

        for (auto const & localCounts : threadSelectedCounts)
        {
            for (auto i = 0ull; i < capacity; ++i)
                selectedCounts[i] += localCounts[i];
        }

        for (auto const & localCounts : threadSampledCounts)
        {
            for (auto i = 0ull; i < capacity; ++i)
                sampledCounts[i] += localCounts[i];
        }

        auto coverage = make_coverage_stats(participatingIds, selectedCounts, sampledCounts);
        print_service_distribution_summary(numThreads, globalSamples, activeSignalCount, coverage);
    }


    //==============================================================================
    template <algorithm Algorithm>
    void benchmark
    (
        std::size_t numThreads,
        std::chrono::nanoseconds testDuration
    )
    {
        benchmark_affinity::print_environment_once(cores, numThreads);
        print_service_distribution_configuration_once(testDuration, numThreads);

        std::cout << "==================================================================================\n";
        std::cout << "Algorithm: " << algorithm_name<Algorithm>() << "\n";
        print_service_distribution_header();

        for (auto i = 1ull; i <= numThreads; ++i)
            run_benchmark<Algorithm>(i, testDuration);
    }


} // namespace service_distance_benchmark
