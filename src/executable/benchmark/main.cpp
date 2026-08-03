// work_contract throughput benchmark (signal_tree rewrite).
//
//   usage: benchmark [duration_ms] [thread_counts_csv]
//     duration_ms         wall-clock window per data point   (default 500)
//     thread_counts_csv   e.g. "1,2,4,8,16"                  (default 1..hw)
//
// Sweeps worker thread counts across four task weights, for two subtree sizes.
// Output columns match benchmark_baseline exactly so the two can be diffed.

#include "./benchmark_driver.h"
#include "./wc_adapter.h"

#include <cstdio>


int main
(
    int argc,
    char const ** argv
)
{
    using namespace bcpp::concurrency::benchmark;

    auto cfg = parse_config(argc, argv);
    if (cfg.help)
    {
        std::printf("usage: benchmark [--csv] [--pcores|--ecores] [duration_ms] [thread_counts_csv]\n"
                    "  --csv               emit machine-readable rows (see header) instead of a table\n"
                    "  --pcores            run only on performance (SMT) cores\n"
                    "  --ecores            run only on efficiency cores\n"
                    "  duration_ms         wall-clock window per data point   (default 500)\n"
                    "  thread_counts_csv   e.g. \"1,2,4,8,16\"                  (default 1..#cores)\n");
        return 0;
    }

    // pin the orchestrating thread to the first selected core so the task-cost
    // calibration reflects the same core kind the workers will run on.
    if (not cfg.cores.empty())
        set_cpu_affinity(cfg.cores.front());

    if (cfg.csv)
    {
        std::printf("%s\n", csv_header());
    }
    else
    {
        std::printf("================================================================================\n");
        std::printf("work_contract throughput benchmark  (signal_tree rewrite)\n");
        std::printf("  hardware threads     = %u\n", std::thread::hardware_concurrency());
        std::printf("  core set             = %s (%zu cores, one thread per core)\n", name_of(cfg.selection), cfg.cores.size());
        std::printf("  duration/point       = %lld ms\n", (long long)cfg.duration.count());
        std::printf("  contracts            = %zu\n", max_tasks);
        std::printf("================================================================================\n");
    }

    // 64 signals/subtree is the minimum-latency size the pre-rewrite library used,
    // so it is the apples-to-apples comparison against the baseline.
    run_all<wc_adapter<64>>(cfg.duration, cfg.threadCounts, cfg.cores, cfg.csv);

    // 512 signals/subtree: the rewrite's general-purpose default, shown for contrast.
    run_all<wc_adapter<512>>(cfg.duration, cfg.threadCounts, cfg.cores, cfg.csv);

    return 0;
}
