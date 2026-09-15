// demo 1 - basic contract execution

#include <library/work_contract/work_contract_group.h>

#include <iostream>


int main()
{
    bcpp::concurrency::work_contract_group workContractGroup({.capacity_ = 256});

    auto counter = 0ull;

    // a contract which reschedules itself from within its own work function
    auto recurrentContract = workContractGroup.create_contract(
            [&]()
            {
                std::cout << "recurrent contract, invocation " << ++counter << "\n";
                if (counter < 3)
                    bcpp::concurrency::this_contract::schedule();
            },
            bcpp::concurrency::work_contract::initial_state::scheduled);

    // a plain contract, scheduled on demand
    auto contract = workContractGroup.create_contract(
            [&]()
            {
                std::cout << "second contract\n";
            });
    contract.schedule();

    while (workContractGroup.execute_next_contract())
        ;

    std::cout << "contract valid = " << std::boolalpha << (bool)contract << "\n";
    contract.release();
    while (workContractGroup.execute_next_contract())
        ;
    std::cout << "contract valid after release = " << std::boolalpha << (bool)contract << "\n";

    return 0;
}
