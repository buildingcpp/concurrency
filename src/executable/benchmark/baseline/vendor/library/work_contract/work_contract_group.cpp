#include "./work_contract_group.h"


//=============================================================================
template <bcpp::synchronization_mode T>
bcpp::implementation::work_contract_group<T>::work_contract_group
(
    std::uint64_t capacity
):
    subTreeCount_(minimum_power_of_two((capacity + (signal_tree_type::capacity - 1)) / signal_tree_type::capacity)),
    subTreeMask_(subTreeCount_ - 1),
    subTreeShift_(minimum_bit_count(signal_tree_type::capacity - 1)),
    signalTree_(subTreeCount_),
    available_(subTreeCount_),
    contracts_(subTreeCount_ * signal_tree_type::capacity),
    release_(contracts_.size()),
    exception_(contracts_.size()),
    releaseToken_(subTreeCount_ * signal_tree_type::capacity)
{
    for (auto & subtree : available_)
        for (auto i = 0ull; i < signal_tree_type::capacity; ++i)
            subtree.set(i);
}


//=============================================================================
template <bcpp::synchronization_mode T>
bcpp::implementation::work_contract_group<T>::work_contract_group
(
):
    work_contract_group(default_capacity)
{
}


//=============================================================================
template <bcpp::synchronization_mode T>
bcpp::implementation::work_contract_group<T>::~work_contract_group
(
)
{
    stop();
}


//=============================================================================
template <bcpp::synchronization_mode T>
void bcpp::implementation::work_contract_group<T>::stop
(
)
{
    if (bool wasRunning = !stopped_.exchange(true); wasRunning)
    {
        for (auto & releaseToken : releaseToken_)
            if ((bool)releaseToken)
                releaseToken->orphan();
        if constexpr (mode == synchronization_mode::blocking)
        {
            // this addresses the problem of stopping the group while worker threads
            // are waiting indefinitely for a contract to be scheduled.  Since
            // the group is stopped no such scheduling will ever happen so we
            // give any waiting worker threads a chance to give up the wait now.
            std::unique_lock uniqueLock(mutex_);
            waitableState_.notify_all();
        }
    }
}


//=============================================================================
template <bcpp::synchronization_mode T>
auto bcpp::implementation::work_contract_group<T>::get_available_contract
(
) -> work_contract_id
{
    for (auto i = 0ull; i < available_.size(); ++i)
    {
        auto subTreeIndex (nextAvailableTreeIndex_++ & subTreeMask_);
        if (!available_[subTreeIndex].empty())
        {
            if (auto [signalIndex, _] = available_[subTreeIndex].select<largest_child_selector>(0); signalIndex != ~0ull)
            {
                work_contract_id workContractId(subTreeIndex * signal_tree_capacity);
                workContractId += signalIndex;
                return workContractId;
            }
        }
    }
    return ~0ull; 
}


//=============================================================================
template <bcpp::synchronization_mode T>
void bcpp::implementation::work_contract_group<T>::erase_contract
(
    // after contract's release function is invoked, clean up anything related to the contract
    work_contract_id contractId
) noexcept
{
    auto & contract = contracts_[contractId];
    contract.work_ = nullptr;
    release_[contractId] = nullptr;
    exception_[contractId] = nullptr;
    if (auto releaseToken = std::exchange(releaseToken_[contractId], nullptr); releaseToken)
        releaseToken->orphan(); // mark as invalid

    auto [treeIndex, signalIndex] = get_tree_and_signal_index(contractId);
    available_[treeIndex].set(signalIndex);
}


//=============================================================================
template <bcpp::synchronization_mode T>
void bcpp::implementation::work_contract_group<T>::process_exception
(
    work_contract_id contractId,
    std::exception_ptr exception
)
{
    if (exception_[contractId])
    {
    //    work_contract_token workContractToken(contractId, *this);
        exception_[contractId](/*workContractToken, */exception);
    }
    else
    {
        std::rethrow_exception(exception);
    }
}


//=============================================================================
template <bcpp::synchronization_mode T>
void bcpp::implementation::work_contract_group<T>::process_release
(
    // invoke the contract's release function.  use auto class to ensure
    // erasure of contract in the event of exceptions in the release function.
    work_contract_id contractId
)
{
    auto_erase_contract autoEraseContract(contractId, *this);
    try
    {
        release_[contractId]();
    }
    catch (std::exception const & exception)
    {
        process_exception(contractId, std::current_exception());
    }
}


//=============================================================================
template <bcpp::synchronization_mode T>
bcpp::implementation::work_contract_group<T>::release_token::release_token
(
    work_contract_group * workContractGroup
):
    workContractGroup_(workContractGroup)
{
}


//=============================================================================
template <bcpp::synchronization_mode T>
bool bcpp::implementation::work_contract_group<T>::release_token::schedule
(
    work_contract_type const & workContract
)
{
    std::lock_guard lockGuard(mutex_);
    if (auto workContractGroup = std::exchange(workContractGroup_, nullptr); workContractGroup != nullptr)
    {
        workContractGroup->release(workContract.get_id());
        return true;
    }
    return false;
}


//=============================================================================
template <bcpp::synchronization_mode T>
void bcpp::implementation::work_contract_group<T>::release_token::orphan
(
)
{
    std::lock_guard lockGuard(mutex_);
    workContractGroup_ = nullptr;
}


//=============================================================================
template <bcpp::synchronization_mode T>
bool bcpp::implementation::work_contract_group<T>::release_token::is_valid
(
) const
{
    std::lock_guard lockGuard(mutex_);
    return ((bool)workContractGroup_);
}


//=============================================================================
template <bcpp::synchronization_mode T>
class bcpp::implementation::work_contract_group<T>::auto_erase_contract
{
public:
    auto_erase_contract(std::uint64_t contractId, work_contract_group<T> & owner):contractId_(contractId),owner_(owner){}
    ~auto_erase_contract(){owner_.erase_contract(contractId_);}
private:
    std::uint64_t                   contractId_;
    work_contract_group<T> &         owner_;
};


//=============================================================================
namespace bcpp::implementation
{
    template class work_contract_group<synchronization_mode::blocking>;
    template class work_contract_group<synchronization_mode::non_blocking>;
}