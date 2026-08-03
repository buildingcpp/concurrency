#include "test_support.h"

#include <library/signal_tree/densest_child_selector.h>
#include <library/signal_tree/signal_tree.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <type_traits>
#include <vector>


namespace
{

    constexpr auto value(bcpp::concurrency::signal_id signal) noexcept
    {
        return static_cast<bcpp::concurrency::signal_id::value_type>(signal);
    }


    static_assert(bcpp::concurrency::signal_tree<0>::capacity == 64);
    static_assert(bcpp::concurrency::signal_tree<1>::capacity == 512);
    static_assert(bcpp::concurrency::signal_tree<2>::capacity == 2'048);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<0>::node_count == 1);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<1>::level_node_count<0>() == 8);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<1>::level_offset<1>() == 8);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<1>::node_count == 9);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<2>::level_node_count<0>() == 32);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<2>::level_offset<1>() == 32);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<2>::level_node_count<1>() == 4);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<2>::level_offset<2>() == 36);
    static_assert(bcpp::concurrency::detail::signal_tree_layout<2>::node_count == 37);
    static_assert(alignof(bcpp::concurrency::signal_tree<0>) == 64);
    static_assert(alignof(bcpp::concurrency::signal_tree<1>) == 64);
    static_assert(alignof(bcpp::concurrency::signal_tree<2>) == 64);
    static_assert(sizeof(bcpp::concurrency::signal_tree<0>) == 64);
    static_assert(sizeof(bcpp::concurrency::signal_tree<1>) == 576);
    static_assert(sizeof(bcpp::concurrency::signal_tree<2>) == 2'368);
    static_assert(std::is_standard_layout_v<bcpp::concurrency::signal_tree<0>>);
    static_assert(std::is_standard_layout_v<bcpp::concurrency::signal_tree<1>>);
    static_assert(std::is_standard_layout_v<bcpp::concurrency::signal_tree<2>>);
    static_assert(std::same_as<decltype(bcpp::concurrency::signal_tree{}), bcpp::concurrency::signal_tree<1>>);
    static_assert(bcpp::concurrency::selector_concept<bcpp::concurrency::fairness_selector>);
    static_assert(bcpp::concurrency::selector_concept<bcpp::concurrency::densest_child_selector>);


    void signal_id_basics()
    {
        auto const invalid = bcpp::concurrency::signal_id{};
        auto const alsoInvalid = bcpp::concurrency::signal_id::invalid();
        auto const first = bcpp::concurrency::signal_id{0};
        auto const second = bcpp::concurrency::signal_id{1};

        test_support::require(not invalid.valid(), "default signal_id must be invalid");
        test_support::require(invalid == alsoInvalid, "invalid signal ids must compare equal");
        test_support::require(first.valid(), "constructed signal_id must be valid");
        test_support::require(first < second, "signal ids must preserve numeric ordering");
        test_support::require(std::hash<bcpp::concurrency::signal_id>{}(second) == std::hash<std::uint64_t>{}(1),
                "signal_id hash must use its numeric value");
    }


    template <std::size_t N>
    void basic_transitions()
    {
        bcpp::concurrency::signal_tree<N> tree;
        auto hint = bcpp::concurrency::signal_id{7};

        test_support::require(tree.empty(), "new signal tree must be empty");
        test_support::require(not tree.select(hint).valid(),
                "empty signal tree must not select a signal");
        test_support::require(not hint.valid(), "failed selection must invalidate the hint");

        auto const signal = bcpp::concurrency::signal_id{bcpp::concurrency::signal_tree<N>::capacity / 2};
        test_support::require(tree.set(signal), "first set must publish the signal");
        test_support::require(not tree.set(signal), "duplicate set must be idempotent");
        test_support::require(not tree.empty(), "published signal must make the tree nonempty");

        hint = bcpp::concurrency::signal_id{0};
        test_support::require(tree.select(hint) == signal, "selection must return the published signal");
        test_support::require(tree.empty(), "selection must clear the signal");
        test_support::require(tree.set(signal), "a selected signal must be publishable again");

        hint = bcpp::concurrency::signal_id{0};
        test_support::require(tree.select(hint) == signal,
                "republished signal must be selectable");
        test_support::require(tree.empty(), "republished signal must be cleared");
    }


    template <std::size_t N>
    void fill_and_drain()
    {
        constexpr auto capacity = bcpp::concurrency::signal_tree<N>::capacity;
        bcpp::concurrency::signal_tree<N> tree;

        for (std::uint64_t index = 0; index < capacity; ++index)
            test_support::require(tree.set(bcpp::concurrency::signal_id{index}),
                    "every clear signal must be publishable");

        for (std::uint64_t index = 0; index < capacity; ++index)
            test_support::require(not tree.set(bcpp::concurrency::signal_id{index}),
                    "full-tree duplicate set must fail");

        std::vector<unsigned char> seen(capacity, 0);
        auto hint = bcpp::concurrency::signal_id{0};

        for (std::size_t selectedCount = 0; selectedCount < capacity; ++selectedCount)
        {
            auto const selected = tree.select(hint);
            test_support::require(selected.valid(), "full tree must yield exactly capacity selections");
            auto const selectedValue = value(selected);
            test_support::require(selectedValue < capacity, "selected signal must be within tree capacity");
            test_support::require(seen[selectedValue]++ == 0, "a signal must not be selected twice");
        }

        test_support::require(not tree.select(hint).valid(),
                "drained tree must not yield another signal");
        test_support::require(tree.empty(), "drained tree must report empty");

        for (auto count : seen)
            test_support::require(count == 1, "every published signal must be selected exactly once");
    }


    template <std::size_t N>
    void branch_selection_wraps_to_ready_work()
    {
        constexpr auto capacity = bcpp::concurrency::signal_tree<N>::capacity;
        bcpp::concurrency::signal_tree<N> tree;
        auto const signal = bcpp::concurrency::signal_id{3};
        auto hint = bcpp::concurrency::signal_id{capacity - 2};

        test_support::require(tree.set(signal), "sparse signal must be publishable");
        test_support::require(tree.select(hint) == signal,
                "a branch tree must reach ready work preceding the initial hint");
        test_support::require(tree.empty(), "sparse selection must drain the tree");
    }


    template <typename MakeTree>
    void require_leaf_selection
    (
        std::string_view caseName,
        MakeTree makeTree,
        std::initializer_list<std::uint64_t> ready,
        std::uint64_t initialHint,
        std::initializer_list<std::uint64_t> expected
    )
    {
        auto tree = makeTree();

        for (auto signal : ready)
            test_support::require(tree.set(bcpp::concurrency::signal_id{signal}), caseName);

        auto hint = bcpp::concurrency::signal_id{initialHint};

        for (auto selected = expected.begin(); selected != expected.end(); ++selected)
        {
            test_support::require(tree.select(hint) == bcpp::concurrency::signal_id{*selected}, caseName);

            auto const next = selected + 1;
            if ((next != expected.end()) && (*selected < *next))
                test_support::require(hint == bcpp::concurrency::signal_id{*next}, caseName);
        }

        test_support::require(not tree.select(hint).valid(), caseName);
        test_support::require(tree.empty(), caseName);
    }


    void leaf_selection_matrix()
    {
        constexpr auto capacity = bcpp::concurrency::signal_tree<0>::capacity;
        auto makeTree = [] { return bcpp::concurrency::signal_tree<0>{}; };

        require_leaf_selection("one signal, hint before", makeTree, {3}, 2, {3});
        require_leaf_selection("one signal, hint at", makeTree, {3}, 3, {3});
        require_leaf_selection("one signal, hint after", makeTree, {3}, capacity - 2, {3});
        require_leaf_selection("two signals, hint before", makeTree, {3, 5}, 2, {3, 5});
        require_leaf_selection("two signals, hint at first", makeTree, {3, 5}, 3, {3, 5});
        require_leaf_selection("two signals, hint between", makeTree, {3, 5}, 4, {5, 3});
        require_leaf_selection("two signals, hint at second", makeTree, {3, 5}, 5, {5, 3});
        require_leaf_selection("two signals, hint after", makeTree, {3, 5}, capacity - 2, {3, 5});
    }


    template <std::size_t N>
    void densest_selector_prefers_the_densest_root_child()
    {
        constexpr auto childCapacity = bcpp::concurrency::node_traits<N - 1>::node_capacity;
        constexpr auto denseChildBase = bcpp::concurrency::signal_tree<N>::capacity - childCapacity;
        bcpp::concurrency::signal_tree<N> tree;

        test_support::require(tree.set(bcpp::concurrency::signal_id{0}),
                "sparse child signal must be publishable");
        test_support::require(tree.set(bcpp::concurrency::signal_id{denseChildBase}),
                "dense child signal must be publishable");
        test_support::require(tree.set(bcpp::concurrency::signal_id{denseChildBase + 1}),
                "dense child signal must be publishable");
        test_support::require(tree.set(bcpp::concurrency::signal_id{denseChildBase + 2}),
                "dense child signal must be publishable");

        auto hint = bcpp::concurrency::signal_id{0};
        auto const selected = tree.template select<bcpp::concurrency::densest_child_selector>(hint);
        test_support::require(value(selected) >= denseChildBase,
                "densest selector must choose the root child containing more signals");
    }

} // namespace


int main()
{
    return test_support::run_suite([]
    {
        test_support::run("signal_id basics", signal_id_basics);
        test_support::run("signal_tree<0> basic transitions", basic_transitions<0>);
        test_support::run("signal_tree<1> basic transitions", basic_transitions<1>);
        test_support::run("signal_tree<2> basic transitions", basic_transitions<2>);
        test_support::run("signal_tree<0> full capacity", fill_and_drain<0>);
        test_support::run("signal_tree<1> full capacity", fill_and_drain<1>);
        test_support::run("signal_tree<2> full capacity", fill_and_drain<2>);
        test_support::run("signal_tree<0> leaf selection matrix", leaf_selection_matrix);
        test_support::run("signal_tree<1> branch wrap", branch_selection_wraps_to_ready_work<1>);
        test_support::run("signal_tree<2> branch wrap", branch_selection_wraps_to_ready_work<2>);
        test_support::run("signal_tree<1> densest selector", densest_selector_prefers_the_densest_root_child<1>);
        test_support::run("signal_tree<2> densest selector", densest_selector_prefers_the_densest_root_child<2>);
    });
}
