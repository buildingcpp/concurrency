#pragma once

// Adapter binding the benchmark driver to the current (signal_tree rewrite) API.

#include <library/work_contract/work_contract_group.h>

#include <cstddef>
#include <utility>
#include <vector>


namespace bcpp::benchmark
{

    template <std::size_t Subtree>
    struct wc_adapter
    {
        static constexpr char const *   build_name = "work_contract (signal_tree rewrite)";
        static constexpr char const *   build_id = "rewrite";
        static constexpr std::size_t    subtree = Subtree;

        explicit wc_adapter(std::size_t capacity) :
            group_({.capacity_ = capacity})
        {
            handles_.reserve(capacity);
        }

        template <typename F>
        void create_scheduled(F && f)
        {
            handles_.push_back(group_.create_contract(
                std::forward<F>(f), work_contract::initial_state::scheduled));
        }

        bool execute()
        {
            return group_.execute_next_contract();
        }

        work_contract_group<Subtree>    group_;
        std::vector<work_contract>      handles_;
    };

} // namespace bcpp::benchmark
