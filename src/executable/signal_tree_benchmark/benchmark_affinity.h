#pragma once

#if !defined(__linux__) && defined(BCPP_REQUIRE_CPU_AFFINITY)
#error "BCPP_REQUIRE_CPU_AFFINITY requires Linux CPU affinity support"
#endif

#if defined(__linux__)
#include <library/system.h>
#endif

#include <cstddef>
#include <iostream>


namespace benchmark_affinity
{

    inline constexpr bool enabled =
#if defined(__linux__)
        true;
#else
        false;
#endif

    inline constexpr auto ansi_bold  = "\033[1m";
    inline constexpr auto ansi_reset = "\033[0m";


    inline char const * policy() noexcept
    {
#if defined(__linux__)
        return "linux";
#else
        return "disabled";
#endif
    }


    inline void set_cpu_affinity(int cpuId) noexcept
    {
#if defined(__linux__)
        bcpp::concurrency::system::set_cpu_affinity(cpuId);
#else
        (void)cpuId;
#endif
    }


    inline void print_cpu_list(int const * cpuIds, std::size_t count, std::ostream & os = std::cout)
    {
        os << "affinity: " << policy() << "\n";
        os << "cpus: ";

        if (not enabled)
        {
            os << "not pinned";
        }
        else
        {
            for (auto i = std::size_t{0}; i < count; ++i)
            {
                if (i != 0)
                    os << ',';

                os << cpuIds[i];
            }
        }

        os << "\n";
        os << "topology_validation: external\n";
    }


    inline void print_environment_once(int const * cpuIds, std::size_t count, std::ostream & os = std::cout)
    {
        static auto printed = false;

        if (printed)
            return;

        printed = true;

        os << "==================================================================================\n";
        os << ansi_bold;
        os << "Benchmark Environment\n";
        print_cpu_list(cpuIds, count, os);
        os << ansi_reset;
    }

} // namespace benchmark_affinity
