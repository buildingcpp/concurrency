#include "./test_support.h"

#include <library/work_contract/work_contract_group.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


namespace
{
    struct work_error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    struct release_error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
}


//=============================================================================
void test_schedule_coalescing(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    std::uint64_t runs{};
    auto contract = group.create_contract([&] { ++runs; });

    for (auto count = 0; count < 100; ++count)
        contract.schedule();

    suite.check(wc_test::drain(group) == 1, "repeated pending schedules coalesce to one execution");
    suite.check(runs == 1, "coalesced scheduling invoked the work once");

    contract.schedule();
    suite.check(wc_test::drain(group) == 1 && runs == 2, "a later schedule starts a new execution cycle");
}


//=============================================================================
void test_callable_state_persists(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    std::vector<int> observations;
    auto contract = group.create_contract(
            [count = 0, &observations]() mutable
            {
                observations.push_back(++count);
            });

    for (auto expected = 1; expected <= 3; ++expected)
    {
        contract.schedule();
        suite.check(group.execute_next_contract(), "a stateful callable can be scheduled again");
    }

    suite.check(observations == std::vector<int>({1, 2, 3}), "callable state persists across executions");
}


//=============================================================================
void test_release_semantics(wc_test::suite & suite)
{
    std::uint64_t released{};
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});

    std::uint64_t runs{};
    auto contract = group.create_contract([&] { ++runs; }, [&] {++released;});
    suite.check(contract.release(), "release of an idle live contract is accepted");
    suite.check(contract.release(), "a repeated pending release is idempotently accepted");
    contract.schedule();
    suite.check(contract.is_valid(), "release remains pending until an executor processes it");
    suite.check(wc_test::drain(group) == 1, "release needs one executor action");
    suite.check(runs == 0, "release takes precedence over pending work");
    suite.check(released == 1, "the contract's release handler runs exactly once");
    suite.check(not contract.is_valid(), "processed release invalidates the handle");
    suite.check(not contract.release(), "release of a recycled handle is rejected");
}


//=============================================================================
void test_release_controls_callable_destruction(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = resource;
    auto contract = group.create_contract([resource] {});
    resource.reset();

    suite.check(not lifetime.expired(), "the stored callable owns its captured resources");
    contract.release();
    suite.check(not lifetime.expired(), "requesting release does not destroy the callable synchronously");
    suite.check(group.execute_next_contract(), "an executor processes the resource-owning contract's release");
    suite.check(lifetime.expired(), "processing release destroys the callable and its captures");
}


//=============================================================================
void test_destructor_release(wc_test::suite & suite)
{
    std::uint64_t released{};
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});

    {
        auto contract = group.create_contract([] {}, [&] {++released;});
    }

    suite.check(released == 0, "handle destruction requests, but does not synchronously process, release");
    suite.check(wc_test::drain(group) == 1, "the executor processes a destructor-triggered release");
    suite.check(released == 1, "destruction results in exactly one release callback");
}


//=============================================================================
void test_move_semantics(wc_test::suite & suite)
{
    std::uint64_t sourceReleased{};
    std::uint64_t overwrittenReleased{};
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});

    std::uint64_t runs{};
    auto source = group.create_contract([&] { ++runs; }, [&] {++sourceReleased;});
    bcpp::concurrency::work_contract destination{std::move(source)};

    suite.check(not source.is_valid(), "move construction empties the source handle");
    suite.check(destination.is_valid(), "move construction transfers the contract");
    destination.schedule();
    suite.check(wc_test::drain(group) == 1 && runs == 1, "the move-constructed handle remains operational");

    auto overwritten = group.create_contract([] {}, [&] {++overwrittenReleased;});
    overwritten = std::move(destination);
    suite.check(not destination.is_valid(), "move assignment empties its source");
    suite.check(overwritten.is_valid(), "move assignment transfers the incoming contract");
    suite.check(wc_test::drain(group) == 1, "move assignment asynchronously releases the previous destination");
    suite.check((overwrittenReleased == 1) && (sourceReleased == 0), "the overwritten contract runs its own release handler");

    auto * sameHandle = &overwritten;
    overwritten = std::move(*sameHandle);
    suite.check(overwritten.is_valid(), "self move-assignment preserves the handle");
}


//=============================================================================
void test_self_control_and_context(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    std::uint64_t runs{};
    bcpp::concurrency::work_contract_id observed;

    suite.check(not bcpp::concurrency::this_contract::is_executing(), "this_contract is inactive outside work");

    auto contract = group.create_contract(
            [&]
            {
                ++runs;
                suite.check(bcpp::concurrency::this_contract::is_executing(), "this_contract is active inside work");
                observed = bcpp::concurrency::this_contract::get_id();
                if (runs < 3)
                    bcpp::concurrency::this_contract::schedule();
                else
                    bcpp::concurrency::this_contract::release();
            },
            bcpp::concurrency::work_contract::initial_state::scheduled);
    suite.check(wc_test::drain(group) == 4, "three runs plus terminal self-release were processed");
    suite.check(runs == 3, "self-scheduling produced the requested run count");
    suite.check(observed.valid(), "this_contract exposes a valid internal execution id");
    suite.check(not contract.is_valid(), "self-release eventually invalidates the external handle");
    suite.check(not bcpp::concurrency::this_contract::is_executing(), "this_contract context is cleared after work");
}


//=============================================================================
void test_nested_execution_context(wc_test::suite & suite)
{
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    bcpp::concurrency::work_contract inner;
    bcpp::concurrency::work_contract_id outerId;
    bcpp::concurrency::work_contract_id innerId;
    std::uint64_t innerRuns{};

    auto outer = group.create_contract(
            [&]
            {
                outerId = bcpp::concurrency::this_contract::get_id();
                inner.schedule();
                suite.check(group.execute_next_contract(), "work can synchronously execute another pending contract");
                suite.check(bcpp::concurrency::this_contract::get_id() == outerId, "outer context is restored after nested execution");
            });

    inner = group.create_contract(
            [&]
            {
                ++innerRuns;
                innerId = bcpp::concurrency::this_contract::get_id();
            });

    outer.schedule();
    suite.check(wc_test::drain(group) == 1, "the outer signal was drained");
    suite.check(innerRuns == 1, "the inner contract ran inside the outer callback");
    suite.check(outerId.valid() && innerId.valid() && outerId != innerId, "nested contracts receive distinct internal execution ids");
    suite.check(not bcpp::concurrency::this_contract::is_executing(), "nested execution leaves no stale TLS context");
}


//=============================================================================
void test_work_exception_paths(wc_test::suite & suite)
{
    {
        bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
        auto contract = group.create_contract(
                [] { throw work_error{"unhandled work error"}; },
                bcpp::concurrency::work_contract::initial_state::scheduled);

        suite.throws<work_error>(
                [&] { static_cast<void>(group.execute_next_contract()); },
                "a work exception propagates when no handler is installed");
        suite.check(contract.is_valid(), "a throwing contract remains valid");
        suite.check(not group.execute_next_contract(), "an unhandled exception still clears executing state");
    }

    {
        std::uint64_t caught{};
        std::uint64_t otherCaught{};
        std::string message;
        bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
        auto contract = group.create_contract(
                []
                {
                    throw work_error{"handled work error"};
                },
                [] {},
                [&](std::exception_ptr exception)
                {
                    ++caught;
                    try
                    {
                        std::rethrow_exception(exception);
                    }
                    catch (work_error const & error)
                    {
                        message = error.what();
                    }
                },
                bcpp::concurrency::work_contract::initial_state::scheduled);
        auto other = group.create_contract(
                [] {throw work_error{"other work error"};},
                [] {},
                [&](std::exception_ptr) {++otherCaught;});

        suite.check(group.execute_next_contract(), "a handled throwing contract counts as processed work");
        suite.check(caught == 1, "the exception handler runs exactly once");
        suite.check(otherCaught == 0, "a different contract's exception handler does not run");
        suite.check(message == "handled work error", "the original exception reaches the handler");
        static_cast<void>(other);
    }
}


//=============================================================================
void test_release_exception_paths(wc_test::suite & suite)
{
    {
        bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
        auto contract = group.create_contract(
                [] {},
                [] {throw release_error{"release failed"};});
        contract.release();

        suite.throws<release_error>(
                [&] { static_cast<void>(group.execute_next_contract()); },
                "a release-handler exception propagates without an exception handler");
        suite.check(not contract.is_valid(), "a throwing release handler cannot prevent slot erasure");
    }

    {
        std::uint64_t caught{};
        std::string message;
        bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
        auto contract = group.create_contract(
                [] {},
                [] {throw release_error{"handled release error"};},
                [&](std::exception_ptr exception)
                {
                    ++caught;
                    try
                    {
                        std::rethrow_exception(exception);
                    }
                    catch (release_error const & error)
                    {
                        message = error.what();
                    }
                });
        contract.release();

        suite.check(group.execute_next_contract(), "a handled release error still counts as processed");
        suite.check(caught == 1 && message == "handled release error", "release errors use the contract's exception handler");
        suite.check(not contract.is_valid(), "the contract is erased after a handled release error");
    }
}


//=============================================================================
void test_reschedule_survives_exception(wc_test::suite & suite)
{
    std::uint64_t runs{};
    std::uint64_t caught{};
    bcpp::concurrency::work_contract_group<64> group({.capacity_ = 64});
    auto contract = group.create_contract(
            [&]
            {
                if (++runs == 1)
                {
                    bcpp::concurrency::this_contract::schedule();
                    throw work_error{"retry"};
                }
            },
            [] {},
            [&](std::exception_ptr) {++caught;},
            bcpp::concurrency::work_contract::initial_state::scheduled);

    suite.check(wc_test::drain(group) == 2, "a schedule requested before an exception remains pending");
    suite.check(runs == 2 && caught == 1, "the contract retried once and reported one exception");
    suite.check(contract.is_valid(), "retry after exception does not alter lifetime");
}


//=============================================================================
int main()
{
    wc_test::suite suite{"contract lifecycle and callbacks"};
    test_schedule_coalescing(suite);
    test_callable_state_persists(suite);
    test_release_semantics(suite);
    test_release_controls_callable_destruction(suite);
    test_destructor_release(suite);
    test_move_semantics(suite);
    test_self_control_and_context(suite);
    test_nested_execution_context(suite);
    test_work_exception_paths(suite);
    test_release_exception_paths(suite);
    test_reschedule_survives_exception(suite);
    return suite.finish();
}
