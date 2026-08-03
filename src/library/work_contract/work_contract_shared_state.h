#pragma once

#include "./work_contract_id.h"

#include <library/signal_tree/signal_set.h>
#include <include/synchronization_mode.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>


namespace bcpp::concurrency
{

    class work_contract;

    template <std::size_t, synchronization_mode>
    class work_contract_group;


    template <std::size_t Signals, std::size_t Depth = 0, bool Match = (tree_capacity_v<Depth> == Signals)>
    struct subtree_depth;

    template <std::size_t Signals, std::size_t Depth>
    struct subtree_depth<Signals, Depth, true>
    {
        static constexpr std::size_t value = Depth;
    };

    template <std::size_t Signals, std::size_t Depth>
    struct subtree_depth<Signals, Depth, false>
    {
        static_assert(tree_capacity_v<Depth> < Signals, "invalid signals_per_subtree: not a valid signal_tree size");
        static constexpr std::size_t value = subtree_depth<Signals, Depth + 1>::value;
    };

    template <std::size_t Signals>
    inline constexpr std::size_t subtree_depth_v = subtree_depth<Signals>::value;


    template <std::size_t SignalsPerSubtree, synchronization_mode Mode>
    class work_contract_shared_state
    {
        friend class work_contract;

        template <std::size_t, synchronization_mode>
        friend class work_contract_group;

        static auto constexpr schedule_flag = 0x0000000000000001ull;
        static auto constexpr execute_flag  = 0x0000000000000002ull;
        static auto constexpr release_flag  = 0x0000000000000004ull;

        static auto constexpr flags_width = 16ull;
        static auto constexpr flags_mask  = (1ull << flags_width) - 1ull;
        static auto constexpr generation_mask = ~flags_mask;
        static auto constexpr generation_increment = (1ull << flags_width);

        struct alignas(64) contract_state
        {
            std::atomic<std::uint64_t> state_{};
        };

        using signal_set_type = signal_set<subtree_depth_v<SignalsPerSubtree>, Mode>;
        using available_set_type = signal_set<subtree_depth_v<SignalsPerSubtree>>;

        //=====================================================================
        explicit work_contract_shared_state
        (
            std::uint64_t capacity
        ) :
            signalSet_(capacity),
            available_(capacity),
            contracts_(signalSet_.max() + 1ull)
        {
            for (auto index = 0ull; index < contracts_.size(); ++index)
                available_.set(signal_id{index});
        }


        //=====================================================================
        void add_reference
        (
        ) noexcept
        {
            referenceCount_.fetch_add(1, std::memory_order_relaxed);
        }


        //=====================================================================
        void release_reference
        (
        ) noexcept
        {
            if (referenceCount_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                delete this;
        }


        //=====================================================================
        void schedule
        (
            work_contract_id contractId,
            std::uint64_t generation
        ) noexcept
        {
            auto index = static_cast<std::uint64_t>(contractId);
            if ((not contractId.valid()) || (index >= contracts_.size()))
                return;
            auto & contract = contracts_[index];
            auto state = contract.state_.load(std::memory_order_acquire);

            // Atomically validate the handle generation and set schedule.  Signal
            // only when this call schedules a contract that is not executing;
            // an executing contract is cooperatively signalled during its cleanup.
            for (;;)
            {
                std::uint64_t flags = state & flags_mask;
                auto expected = generation | flags;
                auto next = generation | flags | schedule_flag;
                if (contract.state_.compare_exchange_weak(expected, next, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    if ((flags & (schedule_flag | execute_flag)) == 0)
                        signalSet_.set(contractId.to_signal_id());
                    return;
                }
                state = expected;
                if ((state & generation_mask) != generation)
                    return;
            }
        }


        //=====================================================================
        bool release
        (
            work_contract_id contractId,
            std::uint64_t generation
        ) noexcept
        {
            auto index = static_cast<std::uint64_t>(contractId);
            if ((not contractId.valid()) || (index >= contracts_.size()))
                return false;
            auto & contract = contracts_[index];
            auto state = contract.state_.load(std::memory_order_acquire);

            // Atomically validate the handle generation and request terminal
            // release.  Schedule and signal an idle contract so a worker processes
            // the release; an executing contract is signalled during its cleanup.
            for (;;)
            {
                std::uint64_t flags = state & flags_mask;
                auto nextFlags = flags | release_flag | schedule_flag;
                auto expected = generation | flags;
                auto next = generation | nextFlags;
                if (contract.state_.compare_exchange_weak(expected, next, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    if ((flags & (schedule_flag | execute_flag)) == 0)
                        signalSet_.set(contractId.to_signal_id());
                    return true;
                }
                state = expected;
                if ((state & generation_mask) != generation)
                    return false;
            }
        }


        //=====================================================================
        bool is_valid
        (
            work_contract_id contractId,
            std::uint64_t generation
        ) const noexcept
        {
            auto index = static_cast<std::uint64_t>(contractId);
            return
                (contractId.valid())
                && (index < contracts_.size())
                && ((contracts_[index].state_.load(std::memory_order_acquire)
                    & generation_mask) == generation);
        }


        //=====================================================================
        void invalidate_all
        (
        ) noexcept
        {
            for (auto & contract : contracts_)
            {
                auto state = contract.state_.load(std::memory_order_relaxed);
                auto generation = (state & generation_mask)
                    + generation_increment;
                contract.state_.store(generation, std::memory_order_release);
            }
        }
        signal_set_type              signalSet_;
        available_set_type           available_;
        std::vector<contract_state>  contracts_;

        std::atomic<std::uint64_t>    referenceCount_{1};
    };

} // namespace bcpp::concurrency
