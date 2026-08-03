// work_contract throughput benchmark (pre-rewrite baseline).
//
// Same driver, same output columns as ../main.cpp, built against a frozen
// snapshot of the pre-rewrite work_contract so the two can be diffed directly.
//
//   usage: benchmark_baseline [duration_ms] [thread_counts_csv]

#include "./wc_adapter_baseline.h"   // pulls the frozen pre-rewrite work_contract
#include <benchmark_driver.h>

#include <cstdio>


int main
(
    int argc,
    char const ** argv
)
{
    using namespace bcpp::benchmark;

    auto cfg = parse_config(argc, argv);
    if (cfg.help)
    {
        std::printf("usage: benchmark_baseline [--csv] [--pcores|--ecores] [duration_ms] [thread_counts_csv]\n"
                    "  --csv               emit machine-readable rows (see header) instead of a table\n"
                    "  --pcores            run only on performance (SMT) cores\n"
                    "  --ecores            run only on efficiency cores\n"
                    "  duration_ms         wall-clock window per data point   (default 500)\n"
                    "  thread_counts_csv   e.g. \"1,2,4,8,16\"                  (default 1..#cores)\n");
        return 0;
    }

    if (not cfg.cores.empty())
        set_cpu_affinity(cfg.cores.front());

    if (cfg.csv)
    {
        std::printf("%s\n", csv_header());
    }
    else
    {
        std::printf("================================================================================\n");
        std::printf("work_contract throughput benchmark  (pre-rewrite baseline)\n");
        std::printf("  hardware threads     = %u\n", std::thread::hardware_concurrency());
        std::printf("  core set             = %s (%zu cores, one thread per core)\n", name_of(cfg.selection), cfg.cores.size());
        std::printf("  duration/point       = %lld ms\n", (long long)cfg.duration.count());
        std::printf("  contracts            = %zu\n", max_tasks);
        std::printf("================================================================================\n");
    }

    run_all<wc_baseline_adapter>(cfg.duration, cfg.threadCounts, cfg.cores, cfg.csv);

    return 0;
}
