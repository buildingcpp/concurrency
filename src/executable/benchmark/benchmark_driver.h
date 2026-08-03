#pragma once

// A dependency free, work_contract only throughput benchmark.
//
// Everything that differs between the pre-rewrite and the signal_tree rewrite is
// hidden behind an "adapter" (see wc_adapter.h / baseline/wc_adapter_baseline.h).
// This driver is written against that adapter alone, so the *same* driver measures
// both versions and prints identical columns for a direct diff.
//
// Methodology mirrors the original benchmark: a fixed population of contracts, all
// initially scheduled, each of which re-schedules itself when executed (via
// bcpp::this_contract::schedule) so the group never drains.  Worker threads spin
// on execute_next_contract for a fixed wall-clock window; we report tasks/sec plus
// the coefficient of variation across tasks and across threads as fairness signals.

// the one work_contract symbol the driver needs directly.  resolves to the new
// header or the frozen baseline copy depending on the target's include paths.
#include <library/work_contract/work_contract_this.h>

#include <sched.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>


namespace bcpp::benchmark
{

    // fixed so per-task counters can live in a thread_local array (no hot path alloc)
    static constexpr std::size_t max_tasks = (1u << 13);   // 8192

    // per-thread, per-task execution counts.  summed into globals once, after timing.
    inline thread_local std::array<std::uint64_t, max_tasks> tls_execution_count{};


    //=========================================================================
    inline bool set_cpu_affinity(int core) noexcept
    {
        cpu_set_t cpuSet;
        CPU_ZERO(&cpuSet);
        CPU_SET(core, &cpuSet);
        return (pthread_setaffinity_np(pthread_self(), sizeof(cpuSet), &cpuSet) == 0);
    }


    //=========================================================================
    // parse a Linux cpu list ("0-1,4,6-7") into individual logical cpu ids.
    inline void parse_cpu_list(std::string const & s, std::vector<int> & out)
    {
        std::size_t i = 0;
        while (i < s.size())
        {
            if (s[i] < '0' || s[i] > '9')
            {
                ++i;
                continue;
            }
            auto j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9')
                ++j;
            int lo = std::atoi(s.substr(i, j - i).c_str());
            int hi = lo;
            if (j < s.size() && s[j] == '-')
            {
                auto k = j + 1;
                auto m = k;
                while (m < s.size() && s[m] >= '0' && s[m] <= '9')
                    ++m;
                if (m > k)
                    hi = std::atoi(s.substr(k, m - k).c_str());
                j = m;
            }
            for (auto c = lo; c <= hi; ++c)
                out.push_back(c);
            i = j;
        }
    }


    //=========================================================================
    // which physical cores to run on.  on a hybrid (P/E) part the two kinds are
    // told apart by SMT: a performance core exposes 2+ thread siblings, an
    // efficiency core exposes 1.  'all' keeps one thread per physical core.
    enum class core_selection { all, performance, efficiency };

    inline char const * name_of(core_selection sel) noexcept
    {
        switch (sel)
        {
            case core_selection::performance: return "performance";
            case core_selection::efficiency:  return "efficiency";
            default:                          return "all-physical";
        }
    }


    //=========================================================================
    // representative logical cpu ids -- the lowest-numbered SMT sibling of each
    // physical core -- optionally filtered to a core kind.  no two returned ids
    // share a physical core.  falls back to every logical cpu if topology is
    // unavailable, and to all-physical if a kind filter matches nothing (e.g.
    // asking for E-cores on a non-hybrid part).
    inline std::vector<int> detect_cores(core_selection sel)
    {
        std::vector<int> cores;
        auto hw = std::max(1u, std::thread::hardware_concurrency());
        bool topologyOk = false;
        for (auto cpu = 0u; cpu < hw; ++cpu)
        {
            auto path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu)
                      + "/topology/thread_siblings_list";
            std::ifstream in(path);
            if (not in)
                continue;
            topologyOk = true;
            std::string line;
            std::getline(in, line);
            std::vector<int> siblings;
            parse_cpu_list(line, siblings);
            auto rep = (int)cpu;
            for (auto s : siblings)
                rep = std::min(rep, s);
            if (rep != (int)cpu)                    // keep the lowest sibling only
                continue;
            bool isPerformance = (siblings.size() > 1);   // SMT => P-core
            bool keep = (sel == core_selection::all)
                     || (sel == core_selection::performance && isPerformance)
                     || (sel == core_selection::efficiency && not isPerformance);
            if (keep)
                cores.push_back((int)cpu);
        }
        if (not topologyOk)                         // container / unknown topology
        {
            for (auto c = 0u; c < hw; ++c)
                cores.push_back((int)c);
        }
        else if (cores.empty() && sel != core_selection::all)  // filter matched none
        {
            return detect_cores(core_selection::all);
        }
        return cores;
    }


    //=========================================================================
    // the busy-work each task performs.  N controls task weight / contention.
    template <std::size_t N>
    inline std::size_t hash_task() noexcept
    {
        static constexpr char const * str = "guess what? chicken butt!";
        auto volatile n = std::size_t{0};
        for (auto i = 0ull; i < N; ++i)
            n *= std::hash<std::string_view>()(str);
        return n;
    }


    //=========================================================================
    template <typename Container>
    inline auto gather_stats
    (
        Container const & input,
        std::size_t count
    ) -> std::tuple<std::uint64_t, long double, long double>
    {
        std::uint64_t total = 0;
        for (auto i = 0ull; i < count; ++i)
            total += input[i];
        if (count == 0)
            return {0, 0.0L, 0.0L};
        long double mean = ((long double)total / count);
        long double k = 0;
        for (auto i = 0ull; i < count; ++i)
        {
            long double d = ((long double)input[i] - mean);
            k += (d * d);
        }
        k /= (count > 1 ? (count - 1) : 1);
        long double sd = std::sqrt(k);
        long double cv = (mean != 0) ? (sd / mean) : 0.0L;
        return {total, mean, cv};
    }


    //=========================================================================
    // measure a single task's cost in isolation, for the report header.
    template <typename TaskFn>
    inline double measure_task_ns(TaskFn taskFn) noexcept
    {
        using clock = std::chrono::steady_clock;
        std::uint64_t counter = 0;
        auto start = clock::now();
        auto deadline = start + std::chrono::milliseconds(200);
        do
        {
            for (auto i = 0; i < 1024; ++i)
                (void)taskFn();
            counter += 1024;
        }
        while (clock::now() < deadline);
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
        return ((double)elapsed / (double)counter);
    }


    //=========================================================================
    // Run one (build, weight) data point across a range of worker thread counts.
    // Adapter is the only work_contract-specific dependency.
    // CSV column layout, shared by the header and every data row so they cannot
    // drift apart.
    inline char const * csv_header() noexcept
    {
        return "build,subtree,weight,threads,tasks_per_sec,tasks_per_thread_per_sec,"
               "task_cv,thread_cv,avg_task_ns,duration_ms";
    }

    // weight name -> CSV-safe key (spaces to underscores)
    inline std::string csv_key(char const * name)
    {
        std::string key(name);
        for (auto & c : key)
            if (c == ' ')
                c = '_';
        return key;
    }


    //=========================================================================
    // Run one (build, weight) data point across a range of worker thread counts.
    // Adapter is the only work_contract-specific dependency.
    template <typename Adapter, typename TaskFn>
    inline void run_weight_sweep
    (
        char const * weightName,
        TaskFn taskFn,
        std::size_t taskCount,
        std::chrono::milliseconds duration,
        std::vector<std::size_t> const & threadCounts,
        std::vector<int> const & cores,
        bool csv
    )
    {
        auto avgNs = measure_task_ns(taskFn);

        if (not csv)
        {
            std::printf("\n%s  |  subtree=%zu  |  %s (~%.1f ns/task)\n",
                    Adapter::build_name, Adapter::subtree, weightName, avgNs);
            std::printf("%-10s %-18s %-22s %-10s %-10s\n",
                    "Threads", "Tasks/sec", "Tasks/thread/sec", "Task cv", "Thread cv");
            std::printf("--------------------------------------------------------------------------------\n");
        }

        for (auto numThreads : threadCounts)
        {
            // fresh group per data point so state never carries over
            Adapter harness(taskCount);
            for (auto taskId = 0ull; taskId < taskCount; ++taskId)
            {
                harness.create_scheduled(
                        [taskFn, taskId]()
                        {
                            (void)taskFn();                     // the task
                            bcpp::this_contract::schedule();    // re-arm, like re-queuing
                            ++tls_execution_count[taskId];      // lands in executing thread's array
                        });
            }

            std::atomic<bool> start{false};
            std::atomic<bool> stop{false};
            std::atomic<std::size_t> ready{0};

            std::vector<std::uint64_t> perThreadTotal(numThreads, 0);
            std::vector<std::atomic<std::uint64_t>> perTaskTotal(taskCount);
            for (auto & c : perTaskTotal)
                c.store(0, std::memory_order_relaxed);

            std::vector<std::thread> workers;
            workers.reserve(numThreads);
            for (auto t = 0ull; t < numThreads; ++t)
            {
                workers.emplace_back(
                        [&, t]()
                        {
                            if (not cores.empty())
                                set_cpu_affinity(cores[t % cores.size()]);
                            tls_execution_count.fill(0);
                            ready.fetch_add(1, std::memory_order_release);
                            while (not start.load(std::memory_order_acquire))
                                ;
                            while (not stop.load(std::memory_order_relaxed))
                                harness.execute();
                            // merge this thread's counts into the shared totals
                            std::uint64_t threadTotal = 0;
                            for (auto i = 0ull; i < taskCount; ++i)
                            {
                                auto v = tls_execution_count[i];
                                if (v)
                                    perTaskTotal[i].fetch_add(v, std::memory_order_relaxed);
                                threadTotal += v;
                            }
                            perThreadTotal[t] = threadTotal;
                        });
            }

            while (ready.load(std::memory_order_acquire) != numThreads)
                ;
            auto startTime = std::chrono::steady_clock::now();
            start.store(true, std::memory_order_release);
            std::this_thread::sleep_for(duration);
            stop.store(true, std::memory_order_relaxed);
            for (auto & w : workers)
                w.join();
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            auto seconds = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 1e9;

            auto [taskTotal, taskMean, taskCv] = gather_stats(perTaskTotal, taskCount);
            auto [threadSum, threadMean, threadCv] = gather_stats(perThreadTotal, numThreads);
            (void)taskMean; (void)threadSum; (void)threadMean;

            auto perSec = (seconds > 0) ? (double)taskTotal / seconds : 0.0;
            auto perThreadPerSec = (numThreads > 0) ? (std::uint64_t)(perSec / numThreads) : 0;

            if (csv)
            {
                std::printf("%s,%zu,%s,%zu,%llu,%llu,%.6f,%.6f,%.2f,%lld\n",
                        Adapter::build_id,
                        Adapter::subtree,
                        csv_key(weightName).c_str(),
                        numThreads,
                        (unsigned long long)perSec,
                        (unsigned long long)perThreadPerSec,
                        (double)taskCv,
                        (double)threadCv,
                        avgNs,
                        (long long)duration.count());
            }
            else
            {
                std::printf("%-10zu %-18llu %-22llu %-10.4f %-10.4f\n",
                        numThreads,
                        (unsigned long long)perSec,
                        (unsigned long long)perThreadPerSec,
                        (double)taskCv,
                        (double)threadCv);
            }
            std::fflush(stdout);
        }
    }


    //=========================================================================
    template <typename Adapter>
    inline void run_all
    (
        std::chrono::milliseconds duration,
        std::vector<std::size_t> const & threadCounts,
        std::vector<int> const & cores,
        bool csv = false
    )
    {
        auto constexpr taskCount = max_tasks;
        run_weight_sweep<Adapter>("maximum contention", &hash_task<0>,   taskCount, duration, threadCounts, cores, csv);
        run_weight_sweep<Adapter>("high contention",    &hash_task<1>,   taskCount, duration, threadCounts, cores, csv);
        run_weight_sweep<Adapter>("medium contention",  &hash_task<64>,  taskCount, duration, threadCounts, cores, csv);
        run_weight_sweep<Adapter>("low contention",     &hash_task<256>, taskCount, duration, threadCounts, cores, csv);
    }


    //=========================================================================
    // parse "[--csv] [duration_ms] [thread,counts,csv]" from argv.  the --csv flag
    // may appear anywhere; the two positional args keep their order.
    struct config
    {
        std::chrono::milliseconds   duration{500};
        std::vector<std::size_t>    threadCounts;
        core_selection              selection{core_selection::all};
        std::vector<int>            cores;          // resolved from selection
        bool                        csv{false};
        bool                        help{false};
    };

    inline config parse_config(int argc, char const ** argv)
    {
        config cfg;

        std::vector<std::string> positional;
        for (auto i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "--csv")
                cfg.csv = true;
            else if (a == "--help" || a == "-h")
                cfg.help = true;
            else if (a == "--pcores")
                cfg.selection = core_selection::performance;
            else if (a == "--ecores")
                cfg.selection = core_selection::efficiency;
            else
                positional.push_back(std::move(a));
        }

        cfg.cores = detect_cores(cfg.selection);

        if (positional.size() > 0)
            cfg.duration = std::chrono::milliseconds(std::max(1, std::atoi(positional[0].c_str())));

        if (positional.size() > 1)
        {
            std::string const & csvThreads = positional[1];
            std::size_t pos = 0;
            while (pos < csvThreads.size())
            {
                auto comma = csvThreads.find(',', pos);
                auto token = csvThreads.substr(pos, comma - pos);
                if (not token.empty())
                    cfg.threadCounts.push_back((std::size_t)std::stoul(token));
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
        }
        if (cfg.threadCounts.empty())
        {
            // default sweep spans one thread per selected core
            auto n = cfg.cores.size();
            for (auto t = 1ull; t <= n; ++t)
                cfg.threadCounts.push_back((std::size_t)t);
        }
        return cfg;
    }

} // namespace bcpp::benchmark
