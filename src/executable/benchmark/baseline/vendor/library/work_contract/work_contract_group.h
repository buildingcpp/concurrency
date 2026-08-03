#pragma once

#include "./work_contract_id.h"
#include "./work_contract_this.h"

#include <include/signal_tree.h>
#include <include/synchronization_mode.h>
#include <include/non_movable.h>
#include <include/non_copyable.h>

#include <memory>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <concepts>
#include <bit>


namespace bcpp::implementation
{

    template <synchronization_mode> class work_contract;


    template <synchronization_mode T>
    class work_contract_group final :
        non_copyable,
        non_movable
    {
    public:

        //=============================================================================
        template <std::uint64_t total_counters, std::uint64_t bits_per_counter>
        struct largest_child_selector
        {
            inline auto operator()
            (
                std::uint64_t,
                std::uint64_t counters
            ) const noexcept -> signal_index
            {
                if constexpr (bits_per_counter == 1)
                {
                    return (counters > 0) ? std::countl_zero(counters) : ~0ull;
                }
                else
                {
                    // this routine is only called to select new contract ids.  but we could improve speed here
                    // with a expression fold as total counters is never more than 8 and often 4 or 2.
                    auto selected = ~0ull;
                    auto max = 0ull;
                    /*static*/ auto /*constexpr*/ counter_mask = ((1ull << bits_per_counter) - 1);
                    for (auto i = 0ull; i < total_counters; ++i)
                    {
                        if ((counters & counter_mask) > max)
                        {
                            max = (counters & counter_mask);
                            selected = i;
                        }
                        counters >>= bits_per_counter;
                    }
                    return (total_counters - selected - 1);
                }
            }
        };

        static auto constexpr mode = T;
        using work_contract_type = work_contract<mode>;

        static auto constexpr default_capacity = 512;

        class release_token;

        work_contract_group();

        work_contract_group
        (
            std::uint64_t
        );

        ~work_contract_group();

        work_contract_type create_contract
        (
            std::invocable auto &&,
            work_contract_type::initial_state = work_contract_type::initial_state::unscheduled
        );

        work_contract_type create_contract
        (
            std::invocable auto &&,
            std::invocable auto &&,
            work_contract_type::initial_state = work_contract_type::initial_state::unscheduled
        );

        work_contract_type create_contract
        (
            std::invocable auto &&,
            std::invocable auto &&,
            std::invocable<std::exception_ptr> auto &&,
            work_contract_type::initial_state = work_contract_type::initial_state::unscheduled
        );

        std::uint64_t execute_next_contract();

        std::uint64_t execute_next_contract
        (
            std::uint64_t &
        );

        template <typename rep, typename period>
        std::uint64_t execute_next_contract
        (
            std::chrono::duration<rep, period>
        ) requires (mode == synchronization_mode::blocking);

        template <typename rep, typename period>
        std::uint64_t execute_next_contract
        (
            std::chrono::duration<rep, period>,
            std::uint64_t &
        ) requires (mode == synchronization_mode::blocking);

        void stop();

    private:

        class auto_erase_contract;
        class auto_clear_execute_flag;

        friend class work_contract<mode>;
        friend class release_token;
        friend class auto_erase_contract;
        friend class auto_clear_execute_flag;

        using state_flags = std::uint64_t;

        struct alignas(64) contract
        {
            static auto constexpr release_flag      = 0x00000004ull;
            static auto constexpr execute_flag      = 0x00000002ull;
            static auto constexpr schedule_flag     = 0x00000001ull;

            std::atomic<state_flags>    flags_;
            std::function<void()>       work_;
        };

        void schedule
        (
            work_contract_id
        ) noexcept;

        void release
        (
            work_contract_id
        ) noexcept;

        void set_contract_signal
        (
            work_contract_id
        ) noexcept;

        void process_release(work_contract_id);

        void process_contract(work_contract_id);

        void process_exception(work_contract_id, std::exception_ptr);

        void clear_execute_flag
        (
            work_contract_id
        ) noexcept;

        void erase_contract
        (
            work_contract_id
        ) noexcept;

        work_contract_id get_available_contract();

        std::tuple<std::uint64_t, std::uint64_t> get_tree_and_signal_index
        (
            work_contract_id
        ) const;

        // internal signal tree capacity can be tuned for different performance needs
        static auto constexpr minimum_latency_signal_tree_capacity = 64;
        static auto constexpr general_purpose_signal_tree_capacity = 512;
        static auto constexpr default_signal_tree_capacity = minimum_latency_signal_tree_capacity;

        using signal_tree_type = bcpp::signal_tree<default_signal_tree_capacity>;
        static auto constexpr signal_tree_capacity = signal_tree_type::capacity;

        std::uint64_t                                                   subTreeCount_;

        std::uint64_t                                                   subTreeMask_;

        std::uint64_t                                                   subTreeShift_;

        std::vector<signal_tree_type>                                   signalTree_;

        std::vector<signal_tree_type>                                   available_;

        std::vector<contract>                                           contracts_;

        std::vector<std::function<void()>>                              release_;

        std::vector<std::function<void(std::exception_ptr)>>            exception_;

        std::vector<std::shared_ptr<release_token>>                     releaseToken_;

        std::mutex                                                      mutex_;

        std::atomic<bool>                                               stopped_{false};

        std::atomic<std::uint64_t>                                      nextAvailableTreeIndex_{0};

        static thread_local std::uint64_t                               tls_biasFlags_;

        std::atomic<std::int64_t>                                       nonZeroCounter_{0};

        void decrement_non_zero_counter();
        void increment_non_zero_counter();

        struct
        {
            std::mutex mutable              mutex_;
            std::condition_variable mutable conditionVariable_;

            void notify_all()
            {
                std::lock_guard lockGuard(mutex_);
                conditionVariable_.notify_all();
            }

            bool wait(work_contract_group const * owner) const
            {
                if (owner->nonZeroCounter_ == 0)
                {
                    std::unique_lock uniqueLock(mutex_);
                    conditionVariable_.wait(uniqueLock, [owner](){return ((owner->nonZeroCounter_ != 0) || (owner->stopped_));});
                    return (not owner->stopped_);
                }
                return true;
            }

            bool wait_for
            (
                work_contract_group const * owner,
                std::chrono::nanoseconds duration
            ) const
            {
                if (owner->nonZeroCounter_ == 0)
                {
                    std::unique_lock uniqueLock(mutex_);
                    auto waitSuccess = conditionVariable_.wait_for(uniqueLock, duration, [owner]() mutable{return ((owner->nonZeroCounter_ != 0) || (owner->stopped_));});
                    return ((!owner->stopped_) && (waitSuccess));
                }
                return true;
            }

        } waitableState_;

    }; // class work_contract_group


    //=========================================================================
    template <synchronization_mode T>
    class work_contract_group<T>::release_token final :
        non_copyable,
        non_movable
    {
    public:
        release_token() = delete;
        release_token(work_contract_group *);
        bool schedule(work_contract_type const &);
        void orphan();
        bool is_valid() const;
        std::mutex mutable      mutex_;
        work_contract_group *    workContractGroup_{};
    }; // class work_contract_group<>::release_token


    //=============================================================================
    template <bcpp::synchronization_mode T>
    class bcpp::implementation::work_contract_group<T>::auto_clear_execute_flag
    {
    public:
        auto_clear_execute_flag(std::uint64_t contractId, work_contract_group<T> & owner):contractId_(contractId),owner_(owner){}
        ~auto_clear_execute_flag(){owner_.clear_execute_flag(contractId_);}
    private:
        std::uint64_t               contractId_;
        work_contract_group<T> &    owner_;
    };


    template <synchronization_mode T>
    std::uint64_t thread_local work_contract_group<T>::tls_biasFlags_ = 0;

} // namespace bcpp::implementation


namespace bcpp
{
    //=========================================================================
    using blocking_work_contract_group = implementation::work_contract_group<synchronization_mode::blocking>;
    using work_contract_group = implementation::work_contract_group<synchronization_mode::non_blocking>;

} // namespace bcpp


#include "./work_contract.h"


//=============================================================================
template <bcpp::synchronization_mode T>
inline auto bcpp::implementation::work_contract_group<T>::create_contract
(
    std::invocable auto && workFunction,
    work_contract_type::initial_state initialState
) -> work_contract_type
{
    return create_contract(std::forward<std::decay_t<decltype(workFunction)>>(workFunction), [](){}, initialState);
}

//=============================================================================
template <bcpp::synchronization_mode T>
inline auto bcpp::implementation::work_contract_group<T>::create_contract
(
    std::invocable auto && workFunction,
    std::invocable auto && releaseFunction,
    work_contract_type::initial_state initialState
) -> work_contract_type
{
    return create_contract(std::forward<std::decay_t<decltype(workFunction)>>(workFunction),
            std::forward<decltype(releaseFunction)>(releaseFunction), [](auto){}, initialState);
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline auto bcpp::implementation::work_contract_group<T>::create_contract
(
    std::invocable auto && workFunction,
    std::invocable auto && releaseFunction,
    std::invocable<std::exception_ptr> auto && exceptionFunction,
    work_contract_type::initial_state initialState
) -> work_contract_type
{
    if (auto workContractId = get_available_contract(); workContractId != ~0ull)
    {
        auto & contract = contracts_[workContractId];
        contract.flags_ = 0;
        contract.work_ = std::forward<std::decay_t<decltype(workFunction)>>(workFunction);

        release_[workContractId] = std::forward<decltype(releaseFunction)>(releaseFunction);
        exception_[workContractId] = std::forward<decltype(exceptionFunction)>(exceptionFunction);
        return {this, releaseToken_[workContractId] = std::make_shared<release_token>(this), workContractId, initialState};
    }
    return {};
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract_group<T>::increment_non_zero_counter
(
)
{
    if (nonZeroCounter_++ == 0)
        waitableState_.notify_all();
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract_group<T>::decrement_non_zero_counter
(
)
{
    --nonZeroCounter_;
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline auto bcpp::implementation::work_contract_group<T>::get_tree_and_signal_index
(
    work_contract_id workContractId
) const -> std::tuple<std::uint64_t, std::uint64_t>
{
    return {workContractId / signal_tree_capacity, workContractId % signal_tree_capacity};
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract_group<T>::release
(
    work_contract_id contractId
) noexcept
{
    static auto constexpr flags_to_set = (contract::release_flag | contract::schedule_flag);
    auto previousFlags = contracts_[contractId].flags_.fetch_or(flags_to_set);
    auto notScheduledNorExecuting = ((previousFlags & (contract::schedule_flag | contract::execute_flag)) == 0);
    if (notScheduledNorExecuting)
        set_contract_signal(contractId);
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract_group<T>::schedule
(
    // set the schedule flag.  if not previously set, and not currently executing
    // then also set the signal associated with the contract.
    work_contract_id contractId
) noexcept
{
    static auto constexpr flags_to_set = contract::schedule_flag;
    auto previousFlags = contracts_[contractId].flags_.fetch_or(flags_to_set);
    auto notScheduledNorExecuting = ((previousFlags & (contract::schedule_flag | contract::execute_flag)) == 0);
    if (notScheduledNorExecuting)
        set_contract_signal(contractId);
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract_group<T>::set_contract_signal
(
    // set the signal that is associated with the specified contract
    work_contract_id contractId
) noexcept
{
    if constexpr (mode == synchronization_mode::non_blocking)
    {
        auto [treeIndex, signalIndex] = get_tree_and_signal_index(contractId);
        signalTree_[treeIndex].set(signalIndex);
    }
    else
    {
        auto [treeIndex, signalIndex] = get_tree_and_signal_index(contractId);
        if (auto [treeWasEmpty, success] = signalTree_[treeIndex].set(signalIndex); treeWasEmpty)
        {
            increment_non_zero_counter();
        }
    }
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline std::uint64_t bcpp::implementation::work_contract_group<T>::execute_next_contract
(
    // select a signal (a set signal) from the array of signal trees and, if found,
    // (which clears the signal) then process the pending action on that contract
    // based on the flags associated with that contract.
)
{
    return execute_next_contract(tls_biasFlags_);
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline std::uint64_t bcpp::implementation::work_contract_group<T>::execute_next_contract
(
    // select a signal (a set signal) from the array of signal trees and, if found,
    // (which clears the signal) then process the pending action on that contract
    // based on the flags associated with that contract.
    std::uint64_t & biasFlags
)
{
    if constexpr (mode == synchronization_mode::blocking)
    {
        if (!waitableState_.wait(this))// this should be done more graceful but for now ..
            return ~0ull;
    }

    auto subTreeIndex = (biasFlags / signal_tree_type::capacity);
    for (auto i = 0ull; i < signalTree_.size(); ++i)
    {
        subTreeIndex &= subTreeMask_;
        if (auto [signalIndex, treeIsEmpty] = signalTree_[subTreeIndex].select(biasFlags); signalIndex != invalid_signal_index)
        {
            if constexpr (mode == synchronization_mode::blocking)
            {
                if (treeIsEmpty)
                    decrement_non_zero_counter();
            }
            work_contract_id workContractId(subTreeIndex * signal_tree_capacity);
            workContractId |= signalIndex;
            auto x = (signal_tree::select_bias_hint ^ biasFlags);
            auto b = (x & (~x + 1ull)) & (signal_tree_type::capacity - 1);
            if (b == 0)
            {
                biasFlags = ((subTreeIndex + 1) * signal_tree_type::capacity);
            }
            else
            {
                biasFlags |= b;
                biasFlags &= ~(b - 1);
            }
            process_contract(workContractId);
            return signalIndex;
        }
        biasFlags = (++subTreeIndex * signal_tree_type::capacity);
    }
    return ~0ull;
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract_group<T>::process_contract
(
    work_contract_id contractId
)
{
    auto & contract = contracts_[contractId];
    auto flags = ++contract.flags_;

    if (auto isReleased = ((flags & contract::release_flag) == contract::release_flag); isReleased)
    {
        // release should be far less common path so ensure not inlined
        process_release(contractId);
        return;
    }

    // the expected case. invoke the work function
    auto_clear_execute_flag autoClearExecuteFlag(contractId, *this);

    static constexpr void(*release)(work_contract_id, void *) = [](auto contractId, void * group) noexcept
        {
            reinterpret_cast<work_contract_group<T> *>(group)->release(contractId);
        };
    static constexpr void(*schedule)(work_contract_id, void *) = [](auto contractId, void * group) noexcept
        {
            reinterpret_cast<work_contract_group<T> *>(group)->schedule(contractId);
        };
    bcpp::this_contract thisContract(contractId, this, release, schedule);
    try
    {
        contract.work_();
    }
    catch (...)
    {
        process_exception(contractId, std::current_exception());
    }
}


//=============================================================================
template <bcpp::synchronization_mode T>
template <typename rep, typename period>
inline std::uint64_t bcpp::implementation::work_contract_group<T>::execute_next_contract
(
    // select a signal (a set signal) from the array of signal trees and, if found,
    // (which clears the signal) then process the pending action on that contract
    // based on the flags associated with that contract.
    std::chrono::duration<rep, period> duration
) requires (mode == synchronization_mode::blocking)
{
    return execute_next_contract(duration, tls_biasFlags_);
}


//=============================================================================
template <bcpp::synchronization_mode T>
template <typename rep, typename period>
inline std::uint64_t bcpp::implementation::work_contract_group<T>::execute_next_contract
(
    // select a signal (a set signal) from the array of signal trees and, if found,
    // (which clears the signal) then process the pending action on that contract
    // based on the flags associated with that contract.
    std::chrono::duration<rep, period> duration,
    std::uint64_t & biasFlags
) requires (mode == synchronization_mode::blocking)
{
    if (waitableState_.wait_for(this, duration))
        return this->execute_next_contract(biasFlags);
    return ~0ull;
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract_group<T>::clear_execute_flag
(
    work_contract_id contractId
) noexcept
{
    if (((contracts_[contractId].flags_ -= contract::execute_flag) & contract::schedule_flag) == contract::schedule_flag)
        set_contract_signal(contractId);
}
