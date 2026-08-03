#pragma once

#include "./work_contract_id.h"

#include <include/non_copyable.h>
#include <include/synchronization_mode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>


namespace bcpp
{

    template <std::size_t, synchronization_mode> class work_contract_group;


    //==============================================================================
    class work_contract final :
        non_copyable
    {
    public:

        enum class initial_state
        {
            unscheduled = 0,
            scheduled   = 1
        };

        work_contract() noexcept = default;

        ~work_contract();

        work_contract(work_contract &&) noexcept;
        work_contract & operator = (work_contract &&) noexcept;

        void schedule() noexcept;

        bool release() noexcept;

        bool is_valid() const noexcept;

        explicit operator bool() const noexcept;

    private:

        template <std::size_t, synchronization_mode> friend class work_contract_group;

        using operation = bool (*)(void *, work_contract_id, std::uint64_t) noexcept;
        using operations = std::array<operation, 4>;

        static auto constexpr schedule_operation = 0ull;
        static auto constexpr release_operation = 1ull;
        static auto constexpr is_valid_operation = 2ull;
        static auto constexpr release_reference_operation = 3ull;

        template <typename SharedState>
        static operations const & get_operations() noexcept
        {
            static constexpr operations value
            {
                [](void * state, auto contractId, auto contractGeneration) noexcept
                    {
                        static_cast<SharedState *>(state)->schedule(contractId, contractGeneration);
                        return true;
                    },
                [](void * state, auto contractId, auto contractGeneration) noexcept
                    { return static_cast<SharedState *>(state)->release(contractId, contractGeneration); },
                [](void * state, auto contractId, auto contractGeneration) noexcept
                    { return static_cast<SharedState *>(state)->is_valid(contractId, contractGeneration); },
                [](void * state, auto, auto) noexcept
                    {
                        static_cast<SharedState *>(state)->release_reference();
                        return true;
                    }
            };
            return value;
        }

        void reset() noexcept;

        template <typename SharedState>
        work_contract
        (
            SharedState * sharedState,
            work_contract_id id,
            std::uint64_t generation,
            initial_state initialState
        ) noexcept :
            sharedState_(sharedState),
            operations_(&get_operations<SharedState>()),
            id_(id),
            generation_(generation)
        {
            sharedState->add_reference();
            if (initialState == initial_state::scheduled)
                schedule();
        }

        void *                                          sharedState_{};

        operations const *                              operations_{};

        work_contract_id                                id_;

        std::uint64_t                                   generation_{};

    }; // class work_contract

} // namespace bcpp


//=============================================================================
inline bcpp::work_contract::work_contract
(
    work_contract && other
) noexcept :
    work_contract()
{
    *this = std::move(other);
}


//=============================================================================
inline auto bcpp::work_contract::operator =
(
    work_contract && other
) noexcept -> work_contract &
{
    if (this != &other)
    {
        reset();
        sharedState_ = std::exchange(other.sharedState_, nullptr);
        operations_ = std::exchange(other.operations_, nullptr);
        id_ = std::exchange(other.id_, {});
        generation_ = std::exchange(other.generation_, 0);
    }
    return *this;
}


//=============================================================================
inline bcpp::work_contract::~work_contract
(
)
{
    reset();
}


//=============================================================================
inline void bcpp::work_contract::schedule
(
    // the hot path.  the generation is passed through so that a handle which
    // refers to a slot that has since been recycled is silently ignored.
    //
) noexcept
{
    (*operations_)[schedule_operation](sharedState_, id_, generation_);
}


//=============================================================================
inline bool bcpp::work_contract::release
(
) noexcept
{
    if (sharedState_ == nullptr)
        return false;
    return (*operations_)[release_operation](sharedState_, id_, generation_);
}


//=============================================================================
inline void bcpp::work_contract::reset
(
) noexcept
{
    auto state = std::exchange(sharedState_, nullptr);
    if (state == nullptr)
        return;
    (*operations_)[release_operation](state, id_, generation_);
    (*operations_)[release_reference_operation](state, {}, 0);
}


//=============================================================================
inline bool bcpp::work_contract::is_valid
(
) const noexcept
{
    return
        (sharedState_ != nullptr)
        && ((*operations_)[is_valid_operation](sharedState_, id_, generation_));
}


//=============================================================================
inline bcpp::work_contract::operator bool
(
) const noexcept
{
    return is_valid();
}
