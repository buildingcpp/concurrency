#include "./test_support.h"

#include <library/work_contract/work_contract_group.h>

#include <include/underlying.h>

#include <cstdint>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>


namespace
{
    struct construction_error : std::runtime_error
    {
        construction_error() : std::runtime_error("callable construction failed") {}
    };

    struct throwing_callable
    {
        throwing_callable() = default;
        throwing_callable(throwing_callable const &) { throw construction_error{}; }
        throwing_callable(throwing_callable &&) { throw construction_error{}; }
        void operator () () const {}
    };

    struct move_only_callable
    {
        move_only_callable() : state_(std::make_unique<int>()) {}
        move_only_callable(move_only_callable &&) noexcept = default;
        move_only_callable & operator = (move_only_callable &&) noexcept = default;
        move_only_callable(move_only_callable const &) = delete;
        move_only_callable & operator = (move_only_callable const &) = delete;

        void operator () () {++*state_;}

        std::unique_ptr<int> state_;
    };

    template <typename Group, typename WorkFunction>
    concept can_create_contract = requires (Group & group, WorkFunction && workFunction)
    {
        {group.create_contract(std::forward<WorkFunction>(workFunction))}
            -> std::same_as<bcpp::concurrency::work_contract>;
    };

    template <typename Group>
    concept can_try_execute = requires (Group & group, bcpp::concurrency::signal_id & hint)
    {
        {group.try_execute_next_contract()} -> std::same_as<bool>;
        {group.try_execute_next_contract(hint)} -> std::same_as<bool>;
    };

    static_assert(std::is_default_constructible_v<bcpp::concurrency::work_contract>);
    static_assert(not std::is_copy_constructible_v<bcpp::concurrency::work_contract>);
    static_assert(not std::is_copy_assignable_v<bcpp::concurrency::work_contract>);
    static_assert(std::is_nothrow_move_constructible_v<bcpp::concurrency::work_contract>);
    static_assert(std::is_nothrow_move_assignable_v<bcpp::concurrency::work_contract>);
    static_assert(std::is_nothrow_destructible_v<bcpp::concurrency::work_contract>);

    static_assert(not std::is_copy_constructible_v<bcpp::concurrency::work_contract_group<>>);
    static_assert(not std::is_move_constructible_v<bcpp::concurrency::work_contract_group<>>);

    static_assert(std::invocable<move_only_callable &>);
    static_assert(not std::copy_constructible<move_only_callable>);
    static_assert(not can_create_contract<bcpp::concurrency::work_contract_group<>, move_only_callable>);
    static_assert(can_create_contract<bcpp::concurrency::work_contract_group<>, throwing_callable>);
    static_assert(not can_try_execute<bcpp::concurrency::work_contract_group<>>);
    static_assert(can_try_execute<bcpp::concurrency::blocking_work_contract_group<>>);

    static_assert(bcpp::concurrency::work_contract_group<>::default_capacity == 512);
    static_assert(bcpp::concurrency::work_contract_group<64>::signals_per_subtree_v == 64);
    static_assert(not bcpp::concurrency::work_contract_group<>::blocking);
    static_assert(bcpp::concurrency::blocking_work_contract_group<>::blocking);
    static_assert(std::is_same_v<
            bcpp::concurrency::blocking_work_contract_group<64>,
            bcpp::concurrency::work_contract_group<64, bcpp::synchronization_mode::blocking>>);

    using deduced_group = decltype(bcpp::concurrency::work_contract_group(
            bcpp::concurrency::work_contract_group_configuration{.capacity_ = 64}));
    static_assert(std::is_same_v<deduced_group, bcpp::concurrency::work_contract_group<>>);
}


//=============================================================================
void test_ids(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_id invalid;
    bcpp::concurrency::work_contract_id zero{0};
    bcpp::concurrency::work_contract_id seven{7};
    bcpp::concurrency::work_contract_id fromSignal{bcpp::concurrency::signal_id{7}};

    suite.check(not invalid.valid(), "a default id is invalid");
    suite.check(invalid == bcpp::concurrency::work_contract_id::invalid(), "invalid() returns the sentinel id");
    suite.check(zero.valid(), "id zero is valid");
    suite.check(static_cast<std::uint64_t>(seven) == 7, "an id exposes its numeric value explicitly");
    suite.check(fromSignal == seven, "signal_id conversion preserves the value");
    suite.check(seven.to_signal_id() == bcpp::concurrency::signal_id{7}, "to_signal_id preserves the value");
    suite.check(zero < seven, "ids are ordered");

    std::unordered_set<bcpp::concurrency::work_contract_id> ids{zero, seven, fromSignal};
    suite.check(ids.size() == 2, "std::hash is consistent with id equality");
}


//=============================================================================
void test_default_handle(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract contract;

    suite.check(not contract.is_valid(), "a default handle is invalid");
    suite.check(not static_cast<bool>(contract), "the bool conversion mirrors is_valid()");
    suite.check(not contract.release(), "releasing a default handle is a safe no-op");

    bcpp::concurrency::work_contract moved{std::move(contract)};
    suite.check(not moved.is_valid() && not contract.is_valid(), "moving a default handle keeps both handles invalid");
}


//=============================================================================
void test_capacity(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group defaultGroup;
    bcpp::concurrency::work_contract_group<64> zeroRequested({.capacity_ = 0});
    bcpp::concurrency::work_contract_group<64> oneRequested({.capacity_ = 1});
    bcpp::concurrency::work_contract_group<64> exactRequested({.capacity_ = 64});
    bcpp::concurrency::work_contract_group<64> roundedRequested({.capacity_ = 65});

    suite.check(defaultGroup.capacity() == 512, "the default group has 512 slots");
    suite.check(zeroRequested.capacity() == 64, "zero capacity still allocates one subtree");
    suite.check(oneRequested.capacity() == 64, "capacity rounds up to one subtree");
    suite.check(exactRequested.capacity() == 64, "an exact subtree capacity is preserved");
    suite.check(roundedRequested.capacity() == 128, "capacity rounds up across a subtree boundary");
}


//=============================================================================
void test_creation_and_exhaustion(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 65});
    std::vector<bcpp::concurrency::work_contract> contracts;
    bool allWithinCapacityValid = true;
    contracts.reserve(group.capacity());

    for (auto index = 0ull; index < group.capacity(); ++index)
    {
        auto contract = group.create_contract([] {});
        allWithinCapacityValid &= contract.is_valid();
        contracts.push_back(std::move(contract));
    }

    suite.check(contracts.size() == 128, "all rounded-capacity slots can be allocated");
    suite.check(allWithinCapacityValid, "every creation within capacity returns a valid handle");

    auto overflow = group.create_contract([] {});
    suite.check(not overflow.is_valid(), "creation fails cleanly when the group is full");
    suite.check(not overflow.release(), "a pool-exhaustion handle releases safely");

    suite.check(contracts.back().release(), "a live slot accepts release");
    suite.check(wc_test::drain(group) == 1, "one executor action processes the release");

    auto replacement = group.create_contract([] {});
    suite.check(replacement.is_valid(), "a processed release returns its slot to the pool");
}


//=============================================================================
void test_creation_exception_safety(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    bool exceptionObserved{};
    try
    {
        static_cast<void>(group.create_contract(throwing_callable{}));
    }
    catch (construction_error const &)
    {
        exceptionObserved = true;
    }

    std::vector<bcpp::concurrency::work_contract> contracts;
    contracts.reserve(group.capacity());
    for (auto index = 0ull; index < group.capacity(); ++index)
    {
        auto contract = group.create_contract([] {});
        if (not contract)
            break;
        contracts.push_back(std::move(contract));
    }

    suite.check(exceptionObserved, "a callable-construction exception propagates");
    suite.check(contracts.size() == group.capacity(), "failed callable installation returns its reserved slot");
}


//=============================================================================
void test_initial_state_and_empty_execution(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    std::uint64_t unscheduledRuns{};
    std::uint64_t scheduledRuns{};

    auto unscheduled = group.create_contract([&] { ++unscheduledRuns; });
    suite.check(unscheduled.is_valid(), "an unscheduled contract is valid");
    suite.check(not group.execute_next_contract(), "an unscheduled contract is not executable");
    suite.check(unscheduledRuns == 0, "unscheduled work did not run");

    auto scheduled = group.create_contract(
            [&] { ++scheduledRuns; },
            bcpp::concurrency::work_contract::initial_state::scheduled);
    suite.check(group.execute_next_contract(), "a scheduled initial state is immediately executable");
    suite.check(scheduledRuns == 1, "the initially scheduled work ran once");
    suite.check(not group.execute_next_contract(), "the group reports empty after its signal is consumed");
    suite.check(scheduled.is_valid(), "execution alone does not release a contract");

    unscheduled.schedule();
    suite.check(group.execute_next_contract(), "schedule makes an unscheduled contract executable");
    suite.check(unscheduledRuns == 1, "externally scheduled work ran once");
}


//=============================================================================
int main()
{
    wc_test::suite suite{"public API and capacity"};
    test_ids(suite);
    test_default_handle(suite);
    test_capacity(suite);
    test_creation_and_exhaustion(suite);
    test_creation_exception_safety(suite);
    test_initial_state_and_empty_execution(suite);
    return suite.finish();
}
