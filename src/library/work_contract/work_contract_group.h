#pragma once

#include "./work_contract.h"
#include "./work_contract_id.h"
#include "./work_contract_shared_state.h"
#include "./work_contract_this.h"

#include <library/signal_tree/densest_child_selector.h>
#include <include/non_copyable.h>
#include <include/non_movable.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>


namespace bcpp::concurrency
{

    //==============================================================================
    static constexpr std::uint64_t default_work_contract_group_capacity = 512ull;


    //==============================================================================
    // declared at namespace scope, rather than nested, because neither depends on
    // the subtree size.  that keeps the deduction guides below trivial.
    struct work_contract_group_configuration
    {
        std::uint64_t capacity_{default_work_contract_group_capacity};
    };


    //==============================================================================
    // Lock-free work-contract group backed by signal_tree.  The concurrency model
    // -- thread-safety of each call, the state-word invariants, the ABA/generation
    // design, and the one memory-ordering dependency on signal_tree -- is written
    // up in this library's CONCURRENCY.md.  The load-bearing methods are
    // schedule, release, process_contract and erase_contract below.
    template <std::size_t signals_per_subtree = 512, synchronization_mode Mode = synchronization_mode::non_blocking>
    class work_contract_group :
        non_copyable,
        non_movable
    {
    public:

        static auto constexpr default_capacity = default_work_contract_group_capacity;
        static auto constexpr signals_per_subtree_v = signals_per_subtree;
        static auto constexpr blocking = (Mode == synchronization_mode::blocking);

        using configuration = work_contract_group_configuration;
        work_contract_group
        (
            configuration const & = {}
        );

        ~work_contract_group();

        template <typename WorkFunction>
        requires
        (
            std::invocable<std::decay_t<WorkFunction> &>
            && std::copy_constructible<std::decay_t<WorkFunction>>
        )
        [[nodiscard]] work_contract create_contract
        (
            WorkFunction &&,
            work_contract::initial_state = work_contract::initial_state::unscheduled
        );

        template <typename WorkFunction, typename ReleaseFunction>
        requires
        (
            std::invocable<std::decay_t<WorkFunction> &>
            && std::copy_constructible<std::decay_t<WorkFunction>>
            && std::invocable<std::decay_t<ReleaseFunction> &>
            && std::copy_constructible<std::decay_t<ReleaseFunction>>
        )
        [[nodiscard]] work_contract create_contract
        (
            WorkFunction &&,
            ReleaseFunction &&,
            work_contract::initial_state = work_contract::initial_state::unscheduled
        );

        template <typename WorkFunction, typename ReleaseFunction, typename ExceptionFunction>
        requires
        (
            std::invocable<std::decay_t<WorkFunction> &>
            && std::copy_constructible<std::decay_t<WorkFunction>>
            && std::invocable<std::decay_t<ReleaseFunction> &>
            && std::copy_constructible<std::decay_t<ReleaseFunction>>
            && std::invocable<std::decay_t<ExceptionFunction> &, std::exception_ptr>
            && std::copy_constructible<std::decay_t<ExceptionFunction>>
        )
        [[nodiscard]] work_contract create_contract
        (
            WorkFunction &&,
            ReleaseFunction &&,
            ExceptionFunction &&,
            work_contract::initial_state = work_contract::initial_state::unscheduled
        );

        bool execute_next_contract();

        bool execute_next_contract(signal_id &);

        bool try_execute_next_contract() requires (blocking);

        bool try_execute_next_contract(signal_id &) requires (blocking);

        template <typename Rep, typename Period>
        bool execute_next_contract(std::chrono::duration<Rep, Period> timeout) requires (blocking);

        template <typename Rep, typename Period>
        bool execute_next_contract(signal_id & hint, std::chrono::duration<Rep, Period> timeout) requires (blocking);

        void stop() noexcept;

        std::uint64_t capacity() const noexcept {return work_.size();}

    private:

        friend class work_contract;

        using shared_state = work_contract_shared_state<signals_per_subtree, Mode>;
        using contract_state = typename shared_state::contract_state;
        using available_set = typename shared_state::available_set_type;

        static auto constexpr schedule_flag = shared_state::schedule_flag;
        static auto constexpr execute_flag = shared_state::execute_flag;
        static auto constexpr release_flag = shared_state::release_flag;
        static auto constexpr generation_mask = shared_state::generation_mask;
        static auto constexpr generation_increment = shared_state::generation_increment;

        struct shared_state_deleter
        {
            void operator () (shared_state * state) const noexcept
            {
                state->release_reference();
            }
        };

        class auto_return_available_signal
        {
        public:
            auto_return_available_signal(signal_id id, available_set & available) noexcept :
                id_(id),
                available_(available)
            {
            }

            auto_return_available_signal(auto_return_available_signal const &) = delete;
            auto_return_available_signal & operator = (auto_return_available_signal const &) = delete;

            ~auto_return_available_signal()
            {
                if (id_.valid())
                    available_.set(id_);
            }

            void commit() noexcept
            {
                id_ = {};
            }

        private:
            signal_id       id_;
            available_set & available_;
        };

        class auto_clear_execute_flag
        {
        public:
            auto_clear_execute_flag(work_contract_id id, work_contract_group & owner):id_(id),owner_(owner){}
            ~auto_clear_execute_flag(){owner_.clear_execute_flag(id_);}
        private:
            work_contract_id        id_;
            work_contract_group &   owner_;
        };

        class auto_erase_contract
        {
        public:
            auto_erase_contract(work_contract_id id, work_contract_group & owner):id_(id),owner_(owner){}
            ~auto_erase_contract(){owner_.erase_contract(id_);}
        private:
            work_contract_id        id_;
            work_contract_group &   owner_;
        };

        // generation-validated entry points used by this_contract owner paths.
        void schedule(work_contract_id, std::uint64_t generation) noexcept;
        bool release(work_contract_id, std::uint64_t generation) noexcept;

        // owner-path entry points -- called from within an executing contract (via
        // this_contract), where the generation cannot change under us, so they
        // simply apply against whatever generation the slot currently holds.
        void schedule(work_contract_id) noexcept;
        void release(work_contract_id) noexcept;

        void set_contract_signal(work_contract_id) noexcept;

        void process_contract(work_contract_id);

        void process_release(work_contract_id);

        void process_exception(work_contract_id, std::exception_ptr);

        void clear_execute_flag(work_contract_id) noexcept;

        void erase_contract(work_contract_id) noexcept;

        std::unique_ptr<shared_state, shared_state_deleter> sharedState_;

        std::vector<std::function<void()>>              work_;

        std::vector<std::function<void()>>              release_;

        std::vector<std::function<void(std::exception_ptr)>> exception_;

        std::atomic<bool>                               stopped_{false};

        // the signal_set carries all of the cross subtree bias that the group used
        // to compute by hand.  all that remains here is where this thread left off.
        static thread_local signal_id                   tlsHint_;

    }; // class work_contract_group


    template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
    thread_local signal_id work_contract_group<signals_per_subtree, Mode>::tlsHint_{};


    work_contract_group() -> work_contract_group<>;
    work_contract_group(work_contract_group_configuration const &) -> work_contract_group<>;


    template <std::size_t signals_per_subtree = 512>
    using blocking_work_contract_group = work_contract_group<signals_per_subtree, synchronization_mode::blocking>;

} // namespace bcpp::concurrency


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::work_contract_group
(
    configuration const & config
) :
    sharedState_(new shared_state(config.capacity_)),
    work_(sharedState_->contracts_.size()),
    release_(sharedState_->contracts_.size()),
    exception_(sharedState_->contracts_.size())
{
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::~work_contract_group
(
)
{
    stop();
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::stop
(
) noexcept
{
    if (bool wasRunning = not stopped_.exchange(true); wasRunning)
    {
        if constexpr (blocking)
            sharedState_->signalSet_.stop();
        sharedState_->invalidate_all();
    }
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
template <typename WorkFunction>
requires
(
    std::invocable<std::decay_t<WorkFunction> &>
    && std::copy_constructible<std::decay_t<WorkFunction>>
)
inline auto bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::create_contract
(
    WorkFunction && workFunction,
    work_contract::initial_state initialState
) -> work_contract
{
    return create_contract
    (
        std::forward<WorkFunction>(workFunction),
        [] {},
        [](std::exception_ptr exception) {std::rethrow_exception(exception);},
        initialState
    );
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
template <typename WorkFunction, typename ReleaseFunction>
requires
(
    std::invocable<std::decay_t<WorkFunction> &>
    && std::copy_constructible<std::decay_t<WorkFunction>>
    && std::invocable<std::decay_t<ReleaseFunction> &>
    && std::copy_constructible<std::decay_t<ReleaseFunction>>
)
inline auto bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::create_contract
(
    WorkFunction && workFunction,
    ReleaseFunction && releaseFunction,
    work_contract::initial_state initialState
) -> work_contract
{
    return create_contract
    (
        std::forward<WorkFunction>(workFunction),
        std::forward<ReleaseFunction>(releaseFunction),
        [](std::exception_ptr exception) {std::rethrow_exception(exception);},
        initialState
    );
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
template <typename WorkFunction, typename ReleaseFunction, typename ExceptionFunction>
requires
(
    std::invocable<std::decay_t<WorkFunction> &>
    && std::copy_constructible<std::decay_t<WorkFunction>>
    && std::invocable<std::decay_t<ReleaseFunction> &>
    && std::copy_constructible<std::decay_t<ReleaseFunction>>
    && std::invocable<std::decay_t<ExceptionFunction> &, std::exception_ptr>
    && std::copy_constructible<std::decay_t<ExceptionFunction>>
)
inline auto bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::create_contract
(
    WorkFunction && workFunction,
    ReleaseFunction && releaseFunction,
    ExceptionFunction && exceptionFunction,
    work_contract::initial_state initialState
) -> work_contract
{
    signal_id hint{0};
    auto selected = sharedState_->available_.template select<densest_child_selector>(hint);
    if (not selected.valid())
        return {};
    auto_return_available_signal autoReturnAvailableSignal(selected, sharedState_->available_);

    auto contractId = work_contract_id{selected};
    auto & contract = sharedState_->contracts_[static_cast<std::uint64_t>(contractId)];
    auto generation = contract.state_.load(std::memory_order_acquire) & generation_mask;
    contract.state_.store(generation, std::memory_order_relaxed);
    work_[static_cast<std::uint64_t>(contractId)] = std::forward<WorkFunction>(workFunction);
    release_[static_cast<std::uint64_t>(contractId)] = std::forward<ReleaseFunction>(releaseFunction);
    exception_[static_cast<std::uint64_t>(contractId)] = std::forward<ExceptionFunction>(exceptionFunction);
    autoReturnAvailableSignal.commit();

    return work_contract(sharedState_.get(), contractId, generation, initialState);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline bool bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::execute_next_contract
(
)
{
    return execute_next_contract(tlsHint_);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline bool bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::execute_next_contract
(
    // select a set signal from the signal set (which clears it) and then process
    // whatever the flags say is pending on the contract that it identifies.
    // the hint is where this caller last left off.  it is carried by the caller so
    // that a worker can own its own bias rather than share the thread local one.
    signal_id & hint
)
{
    if constexpr (blocking)
    {
        // signal_set currently exposes parking through its duration overload.
        // Repeat a bounded wait to provide an indefinite public wait without
        // risking overflow in its steady_clock deadline calculation.  stop()
        // wakes the active wait immediately.
        for (;;)
        {
            if (execute_next_contract(hint, std::chrono::hours{24}))
                return true;
            if (stopped_.load(std::memory_order_acquire))
                return false;
        }
    }

    auto selected = sharedState_->signalSet_.select(hint);
    if (not selected.valid())
        return false;
    process_contract(work_contract_id{selected});
    return true;
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline bool bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::try_execute_next_contract
(
) requires (blocking)
{
    return try_execute_next_contract(tlsHint_);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline bool bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::try_execute_next_contract
(
    signal_id & hint
) requires (blocking)
{
    auto const retryFromBeginning = hint.valid() && (hint != signal_id{0});
    auto selected = sharedState_->signalSet_.select(hint);
    if ((not selected.valid()) && retryFromBeginning)
    {
        // A traversal that begins inside a signal-tree leaf intentionally does
        // not wrap within that leaf.  Complete the try from the beginning before
        // reporting that no work is immediately available.
        hint = signal_id{0};
        selected = sharedState_->signalSet_.select(hint);
    }
    if (not selected.valid())
        return false;
    process_contract(work_contract_id{selected});
    return true;
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
template <typename Rep, typename Period>
inline bool bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::execute_next_contract
(
    std::chrono::duration<Rep, Period> timeout
) requires (blocking)
{
    return execute_next_contract(tlsHint_, timeout);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
template <typename Rep, typename Period>
inline bool bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::execute_next_contract
(
    signal_id & hint,
    std::chrono::duration<Rep, Period> timeout
) requires (blocking)
{
    if (timeout <= std::chrono::duration<Rep, Period>::zero())
        return try_execute_next_contract(hint);

    auto selected = sharedState_->signalSet_.select(hint, timeout);
    if (not selected.valid())
        return false;
    process_contract(work_contract_id{selected});
    return true;
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::set_contract_signal
(
    work_contract_id contractId
) noexcept
{
    sharedState_->signalSet_.set(contractId.to_signal_id());
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::schedule
(
    // generation-validated schedule.  set the schedule flag iff the slot still
    // carries the caller's generation; if it was recycled the CAS never commits.
    // if the contract was neither scheduled nor executing, also set its signal.
    work_contract_id contractId,
    std::uint64_t generation
) noexcept
{
    sharedState_->schedule(contractId, generation);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::schedule
(
    // owner-path schedule (from within an executing contract): the generation is
    // whatever the slot holds right now and cannot change under us.
    work_contract_id contractId
) noexcept
{
    auto & contract = sharedState_->contracts_[static_cast<std::uint64_t>(contractId)];
    sharedState_->schedule(contractId,
        contract.state_.load(std::memory_order_acquire) & generation_mask);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline bool bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::release
(
    // generation-validated release.  returns false (and does nothing) if the slot
    // has already been recycled -- i.e. the handle is stale.  otherwise sets the
    // release (and schedule) flag and, if idle, the signal, so a worker runs it.
    work_contract_id contractId,
    std::uint64_t generation
) noexcept
{
    return sharedState_->release(contractId, generation);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::release
(
    // owner-path release (from within an executing contract).
    work_contract_id contractId
) noexcept
{
    auto & contract = sharedState_->contracts_[static_cast<std::uint64_t>(contractId)];
    release(contractId,
        contract.state_.load(std::memory_order_acquire) & generation_mask);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::process_contract
(
    work_contract_id contractId
)
{
    auto & contract = sharedState_->contracts_[static_cast<std::uint64_t>(contractId)];

    // Transition the selected contract from scheduled (001) to executing (010),
    // preserving release and generation.
    // These values guarantee that increment performs exactly that binary carry.
    static_assert(schedule_flag == 1ull);
    static_assert((schedule_flag + 1ull) == execute_flag);
    auto newState = ++contract.state_;

    if (auto isReleased = ((newState & release_flag) == release_flag); isReleased)
    {
        // A selected release request is terminal and does not invoke the work.
        process_release(contractId);
        return;
    }

    // Otherwise execute the work and clear execute cooperatively on scope exit.
    auto_clear_execute_flag autoClearExecuteFlag(contractId, *this);

    static constexpr void(*release)(work_contract_id, void *) = [](auto contractId, void * group) noexcept
        {
            static_cast<work_contract_group *>(group)->release(contractId);
        };
    static constexpr void(*schedule)(work_contract_id, void *) = [](auto contractId, void * group) noexcept
        {
            static_cast<work_contract_group *>(group)->schedule(contractId);
        };
    this_contract thisContract(contractId, this, release, schedule);
    try
    {
        work_[static_cast<std::uint64_t>(contractId)]();
    }
    catch (...)
    {
        process_exception(contractId, std::current_exception());
    }
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::process_release
(
    // notify, then erase.  the auto class ensures erasure even if the handler throws.
    work_contract_id contractId
)
{
    auto_erase_contract autoEraseContract(contractId, *this);
    auto & releaseFunction = release_[static_cast<std::uint64_t>(contractId)];
    if (releaseFunction)
    {
        try
        {
            releaseFunction();
        }
        catch (...)
        {
            process_exception(contractId, std::current_exception());
        }
    }
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::process_exception
(
    work_contract_id contractId,
    std::exception_ptr exception
)
{
    auto & exceptionFunction = exception_[static_cast<std::uint64_t>(contractId)];
    if (exceptionFunction)
        exceptionFunction(exception);
    else
        std::rethrow_exception(exception);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::clear_execute_flag
(
    work_contract_id contractId
) noexcept
{
    auto & contract = sharedState_->contracts_[static_cast<std::uint64_t>(contractId)];

    // Clear execute, then re-signal if cooperative scheduling left schedule set.
    auto previousState = contract.state_.fetch_and(
        ~execute_flag, std::memory_order_acq_rel);
    if ((previousState & schedule_flag) == schedule_flag)
        set_contract_signal(contractId);
}


//=============================================================================
template <std::size_t signals_per_subtree, bcpp::synchronization_mode Mode>
inline void bcpp::concurrency::work_contract_group<signals_per_subtree, Mode>::erase_contract
(
    // clean up the contract and return its slot to the available set.  bumping the
    // generation is what invalidates any work_contract still referring to the slot.
    work_contract_id contractId
) noexcept
{
    auto & contract = sharedState_->contracts_[static_cast<std::uint64_t>(contractId)];
    work_[static_cast<std::uint64_t>(contractId)] = nullptr;
    release_[static_cast<std::uint64_t>(contractId)] = nullptr;
    exception_[static_cast<std::uint64_t>(contractId)] = nullptr;
    auto generation = (contract.state_.load(std::memory_order_relaxed) & generation_mask) + generation_increment;
    contract.state_.store(generation, std::memory_order_release);
    sharedState_->available_.set(contractId.to_signal_id());
}
