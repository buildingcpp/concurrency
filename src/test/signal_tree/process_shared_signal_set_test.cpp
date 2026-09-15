#include "test_support.h"

#include <library/signal_tree.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>


namespace
{

    using namespace std::chrono_literals;

    using signal_set_type = bcpp::concurrency::shared_signal_set<
            0, bcpp::synchronization_mode::blocking>;
    using tree_type = typename signal_set_type::tree_type;


    struct shared_storage
    {
        signal_set_type::shared_state_type blockingState_;
        std::array<tree_type, 2>            trees_;
    };


    //==========================================================================
    void set_in_one_process_wakes_another_process()
    {
        auto * mapping = ::mmap(nullptr, sizeof(shared_storage), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        test_support::require((mapping != MAP_FAILED), "shared test storage must map");

        auto * storage = std::construct_at(static_cast<shared_storage *>(mapping));
        int readyPipe[2];
        auto const pipeResult = ::pipe(readyPipe);
        test_support::require((pipeResult == 0), "process readiness pipe must open");

        auto const child = ::fork();
        test_support::require((child >= 0), "test process must fork");

        if ((child == 0))
        {
            static_cast<void>(::close(readyPipe[0]));

            signal_set_type signals
            {
                std::span<tree_type>{storage->trees_},
                storage->blockingState_
            };
            char const ready = 1;
            static_cast<void>(::write(readyPipe[1], &ready, sizeof(ready)));

            auto hint = bcpp::concurrency::signal_id{0};
            auto const selected = signals.select(hint, 5s);
            auto const expected = bcpp::concurrency::signal_id{tree_type::capacity + 7};
            ::_exit((selected == expected) ? 0 : 1);
        }

        static_cast<void>(::close(readyPipe[1]));

        char ready = 0;
        auto const bytesRead = ::read(readyPipe[0], &ready, sizeof(ready));
        static_cast<void>(::close(readyPipe[0]));

        signal_set_type signals
        {
            std::span<tree_type>{storage->trees_},
            storage->blockingState_
        };

        auto const signal = bcpp::concurrency::signal_id{tree_type::capacity + 7};
        auto const wasSet = ((bytesRead == sizeof(ready)) && (ready == 1))
                ? signals.set(signal)
                : false;
        int childStatus = 0;
        auto const waitedFor = ::waitpid(child, &childStatus, 0);

        std::destroy_at(storage);
        auto const unmapResult = ::munmap(mapping, sizeof(shared_storage));

        test_support::require((bytesRead == sizeof(ready)), "child process must report readiness");
        test_support::require((ready == 1), "child process readiness must be valid");
        test_support::require((wasSet), "parent process must publish the signal");
        test_support::require((waitedFor == child), "parent process must reap the child");
        test_support::require((WIFEXITED(childStatus)), "child process must exit normally");
        test_support::require((WEXITSTATUS(childStatus) == 0),
                "shared timed selection must wake and receive the signal");
        test_support::require((unmapResult == 0), "shared test storage must unmap");
    }

} // namespace


int main()
{
    return test_support::run_suite([]
    {
        test_support::run("process-shared signal-set wake", set_in_one_process_wakes_another_process);
    });
}
