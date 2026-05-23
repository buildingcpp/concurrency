// hardcoding cores to use ... i know, its ugly.  my test machine has
// ecores and I want to make sure that I'm using only those for consistent
// performance during development.
int cores[] = {16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
//int cores[] = {16,20,24,28,17,21,25,29,18,22,26,30,19,23,27,31};

int mainCpu = 0;

#include "./bench_adapter.h"
#include "./benchmark_affinity.h"
#include "./throughput_benchmark.h"
#include "./adversarial_throughput_benchmark.h"
#include "./service_distance_benchmark.h"

#include <chrono>
#include <cstddef>


//==============================================================================
int main(int, char **)
{
    benchmark_affinity::set_cpu_affinity(mainCpu);

    auto numThreads = std::size_t{16};
    auto testDuration = std::chrono::seconds(1);

    // -------- throughput --------
    throughput_benchmark::benchmark<algorithm::moody_camel>(numThreads, testDuration);
    throughput_benchmark::benchmark<algorithm::tbb>        (numThreads, testDuration);
    throughput_benchmark::benchmark<algorithm::lockfree>   (numThreads, testDuration);
    throughput_benchmark::benchmark<algorithm::signal_tree>(numThreads, testDuration);


    // -------- adversarial throughput --------
    adversarial_throughput_benchmark::benchmark<algorithm::moody_camel>(numThreads, testDuration);
    adversarial_throughput_benchmark::benchmark<algorithm::tbb>(numThreads, testDuration);
    adversarial_throughput_benchmark::benchmark<algorithm::lockfree>(numThreads, testDuration);
    adversarial_throughput_benchmark::benchmark<algorithm::signal_tree>(numThreads, testDuration);


    // -------- service distance --------
    service_distance_benchmark::benchmark<algorithm::moody_camel>(numThreads, testDuration);
    service_distance_benchmark::benchmark<algorithm::tbb>        (numThreads, testDuration);
    service_distance_benchmark::benchmark<algorithm::lockfree>   (numThreads, testDuration);
    service_distance_benchmark::benchmark<algorithm::signal_tree>(numThreads, testDuration);

    return 0;
}
