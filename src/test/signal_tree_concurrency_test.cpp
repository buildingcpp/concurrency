#include "test_support.h"

#include <include/signal_tree.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>


namespace
{

    using namespace std::chrono_literals;

    constexpr auto value(bcpp::signal_id signal) noexcept
    {
        return static_cast<bcpp::signal_id::value_type>(signal);
    }


    template <std::size_t N>
    void concurrent_duplicate_publication()
    {
        constexpr std::size_t threadCount = 8;
        constexpr auto capacity = bcpp::signal_tree<N>::capacity;
        bcpp::signal_tree<N> tree;
        std::vector<std::atomic<unsigned>> successfulSets(capacity);
        std::barrier start{threadCount};
        std::vector<std::thread> threads;

        for (auto & count : successfulSets)
            count.store(0, std::memory_order_relaxed);

        for (std::size_t thread = 0; thread < threadCount; ++thread)
            threads.emplace_back([&]
            {
                start.arrive_and_wait();
                for (std::uint64_t index = 0; index < capacity; ++index)
                    if (tree.set(bcpp::signal_id{index}))
                        successfulSets[index].fetch_add(1, std::memory_order_relaxed);
            });

        for (auto & thread : threads)
            thread.join();

        for (auto const & count : successfulSets)
            test_support::require(count.load(std::memory_order_relaxed) == 1,
                    "concurrent duplicate publishers must produce one transition per signal");

        auto hint = bcpp::signal_id{0};
        std::size_t selectedCount = 0;
        while (tree.select(hint).valid())
            ++selectedCount;

        test_support::require(selectedCount == capacity,
                "concurrent duplicate publication must leave every signal ready exactly once");
    }


    template <std::size_t N>
    void concurrent_selection()
    {
        constexpr std::size_t threadCount = 8;
        constexpr auto capacity = bcpp::signal_tree<N>::capacity;
        bcpp::signal_tree<N> tree;
        std::vector<std::atomic<unsigned>> seen(capacity);
        std::atomic<std::size_t> selectedCount{0};
        std::atomic<bool> badSelection{false};
        std::barrier start{threadCount};
        std::vector<std::thread> threads;

        for (auto & count : seen)
            count.store(0, std::memory_order_relaxed);

        for (std::uint64_t index = 0; index < capacity; ++index)
            test_support::require(tree.set(bcpp::signal_id{index}),
                    "concurrent-selection setup must fill the tree");

        for (std::size_t thread = 0; thread < threadCount; ++thread)
            threads.emplace_back([&]
            {
                auto hint = bcpp::signal_id{0};
                start.arrive_and_wait();

                for (;;)
                {
                    auto const selected = tree.select(hint);
                    if (not selected.valid())
                        break;

                    auto const selectedValue = value(selected);
                    if (selectedValue >= capacity)
                    {
                        badSelection.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    if (seen[selectedValue].fetch_add(1, std::memory_order_relaxed) != 0)
                        badSelection.store(true, std::memory_order_relaxed);
                    selectedCount.fetch_add(1, std::memory_order_relaxed);
                }
            });

        for (auto & thread : threads)
            thread.join();

        test_support::require(not badSelection.load(std::memory_order_relaxed),
                "concurrent selectors must return only unique in-range signals");
        test_support::require(selectedCount.load(std::memory_order_relaxed) == capacity,
                "concurrent selectors must collectively drain the tree");
        test_support::require(tree.empty(), "concurrent selectors must leave the tree empty");

        for (auto const & count : seen)
            test_support::require(count.load(std::memory_order_relaxed) == 1,
                    "each signal must be selected exactly once across selecting threads");
    }


    template <std::size_t N>
    void sharded_publication_visibility()
    {
        constexpr std::size_t producerCount = 4;
        constexpr std::size_t consumerCount = 4;
        constexpr auto treeCapacity = bcpp::signal_tree<N>::capacity;
        constexpr auto capacity = treeCapacity * 3;
        constexpr std::uint64_t payloadMask = 0xd1b5'4a32'd192'ed03ull;
        bcpp::signal_set<N> signals{capacity};
        std::vector<std::uint64_t> payload(capacity, 0);
        std::vector<std::atomic<unsigned>> seen(capacity);
        std::atomic<std::size_t> selectedCount{0};
        std::atomic<bool> producersDone{false};
        std::atomic<bool> failure{false};
        std::barrier start{producerCount + consumerCount};
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;

        for (auto & count : seen)
            count.store(0, std::memory_order_relaxed);

        for (std::size_t producer = 0; producer < producerCount; ++producer)
            producers.emplace_back([&, producer]
            {
                start.arrive_and_wait();
                for (std::uint64_t index = producer; index < capacity; index += producerCount)
                {
                    payload[index] = index ^ payloadMask;
                    if (not signals.set(bcpp::signal_id{index}))
                        failure.store(true, std::memory_order_relaxed);
                }
            });

        for (std::size_t consumer = 0; consumer < consumerCount; ++consumer)
            consumers.emplace_back([&]
            {
                auto hint = bcpp::signal_id{0};
                auto const deadline = std::chrono::steady_clock::now() + 15s;
                start.arrive_and_wait();

                while (selectedCount.load(std::memory_order_relaxed) < capacity)
                {
                    auto const selected = signals.select(hint);
                    if (not selected.valid())
                    {
                        if (producersDone.load(std::memory_order_acquire)
                                && std::chrono::steady_clock::now() >= deadline)
                        {
                            failure.store(true, std::memory_order_relaxed);
                            return;
                        }
                        std::this_thread::yield();
                        continue;
                    }

                    auto const selectedValue = value(selected);
                    if (selectedValue >= capacity)
                    {
                        failure.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    if (payload[selectedValue] != (selectedValue ^ payloadMask))
                        failure.store(true, std::memory_order_relaxed);
                    if (seen[selectedValue].fetch_add(1, std::memory_order_relaxed) != 0)
                        failure.store(true, std::memory_order_relaxed);
                    selectedCount.fetch_add(1, std::memory_order_relaxed);
                }
            });

        for (auto & producer : producers)
            producer.join();
        producersDone.store(true, std::memory_order_release);

        for (auto & consumer : consumers)
            consumer.join();

        test_support::require(not failure.load(std::memory_order_relaxed),
                "concurrent sharded publication and selection must preserve payload visibility and uniqueness");
        test_support::require(selectedCount.load(std::memory_order_relaxed) == capacity,
                "concurrent sharded consumers must receive every published signal");

        for (auto const & count : seen)
            test_support::require(count.load(std::memory_order_relaxed) == 1,
                    "every concurrently published sharded signal must be selected once");
    }

} // namespace


int main()
{
    return test_support::run_suite([]
    {
        test_support::run("signal_tree<0> concurrent duplicate publication", concurrent_duplicate_publication<0>);
        test_support::run("signal_tree<1> concurrent duplicate publication", concurrent_duplicate_publication<1>);
        test_support::run("signal_tree<2> concurrent duplicate publication", concurrent_duplicate_publication<2>);
        test_support::run("signal_tree<0> concurrent selection", concurrent_selection<0>);
        test_support::run("signal_tree<1> concurrent selection", concurrent_selection<1>);
        test_support::run("signal_tree<2> concurrent selection", concurrent_selection<2>);
        test_support::run("signal_set<0> sharded publication visibility", sharded_publication_visibility<0>);
        test_support::run("signal_set<1> sharded publication visibility", sharded_publication_visibility<1>);
        test_support::run("signal_set<2> sharded publication visibility", sharded_publication_visibility<2>);
    });
}
