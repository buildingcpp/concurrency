#pragma once

#include <include/synchronization_mode.h>
#include <include/non_copyable.h>

#include <atomic>
#include <cstdint>
#include <utility>
#include <memory>


namespace bcpp::implementation
{

    template <synchronization_mode T>
    class work_contract_group;


    template <synchronization_mode T>
    class alignas(64) work_contract :
        non_copyable
    {
    public:

        using id_type = std::uint64_t;

        enum class initial_state
        {
            unscheduled = 0,
            scheduled = 1
        };

        work_contract() = default;

        ~work_contract();

        work_contract(work_contract &&);
        work_contract & operator = (work_contract &&);

        void schedule();

        bool release();

        bool deschedule();

        bool is_valid() const;

        explicit operator bool() const;

    private:

        friend class work_contract_group<T>;
        using work_contract_group_type = work_contract_group<T>;

        work_contract
        (
            work_contract_group_type *,
            std::shared_ptr<typename work_contract_group_type::release_token>,
            id_type,
            initial_state = initial_state::unscheduled
        );

        id_type get_id() const;

        work_contract_group_type *   owner_{};

        std::atomic<std::shared_ptr<typename work_contract_group_type::release_token>> releaseToken_;

        id_type                 id_{};

    }; // class work_contract

} // namespace bcpp::implementation


namespace bcpp
{

    using work_contract = implementation::work_contract<synchronization_mode::non_blocking>;
    using blocking_work_contract = implementation::work_contract<synchronization_mode::blocking>;
}


#include "./work_contract_group.h"


//=============================================================================
template <bcpp::synchronization_mode T>
inline bcpp::implementation::work_contract<T>::work_contract
(
    work_contract_group_type * owner,
    std::shared_ptr<typename work_contract_group_type::release_token> releaseToken,
    id_type id,
    initial_state initialState
):
    owner_(owner),
    releaseToken_(releaseToken),
    id_(id)
{
    if (initialState == initial_state::scheduled)
        schedule();
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline bcpp::implementation::work_contract<T>::work_contract
(
    work_contract && other
):
    owner_(other.owner_),
    releaseToken_(other.releaseToken_.exchange(nullptr)),
    id_(other.id_)
{
    other.owner_ = {};
    other.id_ = {};
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline auto bcpp::implementation::work_contract<T>::operator =
(
    work_contract && other
) -> work_contract &
{
    if (this != &other)
    {
        release();

        owner_ = other.owner_;
        id_ = other.id_;
        releaseToken_ = other.releaseToken_.exchange(nullptr);
        other.owner_ = {};
        other.id_ = {};
    }
    return *this;
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline bcpp::implementation::work_contract<T>::~work_contract
(
)
{
    release();
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline auto bcpp::implementation::work_contract<T>::get_id
(
) const -> id_type
{
    return id_;
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline void bcpp::implementation::work_contract<T>::schedule
(
)
{
    owner_->schedule(id_);
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline bool bcpp::implementation::work_contract<T>::release
(
)
{
    if (auto releaseToken = releaseToken_.exchange(nullptr); releaseToken)
    {
        releaseToken->schedule(*this);
        owner_ = {};
        return true;
    }
    return false;
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline bool bcpp::implementation::work_contract<T>::is_valid
(
) const
{
    if (auto releaseToken = releaseToken_.load(); releaseToken)
        return releaseToken->is_valid();
    return false;
}


//=============================================================================
template <bcpp::synchronization_mode T>
inline bcpp::implementation::work_contract<T>::operator bool
(
) const
{
    return is_valid();
}
