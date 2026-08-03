// Executable form of the first complete example in INTRODUCTION.md.
// Keeping it in CTest prevents the adoption path from drifting out of sync.

#include <library/work_contract.h>

#include <cstdint>
#include <thread>


int main()
{
    bcpp::work_contract_group group({.capacity_ = 64});
    std::uint64_t invocation = 0;

    auto heartbeat = group.create_contract(
        [&]
        {
            ++invocation;

            if (invocation < 3)
                bcpp::this_contract::schedule();
            else
                bcpp::this_contract::release();
        },
        bcpp::work_contract::initial_state::scheduled);

    std::uint64_t executorActions{};
    while (heartbeat.is_valid() && executorActions < 10)
    {
        if (group.execute_next_contract())
            ++executorActions;
        else
            std::this_thread::yield();
    }

    return not (
        invocation == 3
        && executorActions == 4
        && not heartbeat.is_valid());
}
