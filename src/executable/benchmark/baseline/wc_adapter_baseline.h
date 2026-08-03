#pragma once

// Adapter binding the benchmark driver to the pre-rewrite work_contract API.
// The old headers are resolved from the frozen ./vendor tree (see CMakeLists).

#include <library/work_contract/work_contract_group.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>


namespace bcpp::benchmark
{

    struct wc_baseline_adapter
    {
        static constexpr char const *   build_name = "work_contract (pre-rewrite baseline)";
        static constexpr char const *   build_id = "baseline";
        // the pre-rewrite library used a fixed 64-signal (minimum latency) subtree
        static constexpr std::size_t    subtree = 64;

        explicit wc_baseline_adapter(std::size_t capacity) :
            group_(capacity)
        {
            handles_.reserve(capacity);
        }

        template <typename F>
        void create_scheduled(F && f)
        {
            handles_.push_back(group_.create_contract(std::forward<F>(f), work_contract::initial_state::scheduled));
        }

        bool execute()
        {
            // the pre-rewrite execute_next_contract returns the signal index, or
            // ~0ull when nothing was pending.
            return (group_.execute_next_contract() != ~0ull);
        }

        work_contract_group         group_;
        std::vector<work_contract>  handles_;
    };

} // namespace bcpp::benchmark
