#pragma once

#include <library/signal_tree/signal_tree.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>


namespace test_support
{

    template
    <
        std::size_t tree_size,
        bcpp::concurrency::signal_tree_wrap_policy wrapping =
                bcpp::concurrency::signal_tree_wrap_policy::wrap
    >
    using signal_tree = bcpp::concurrency::signal_tree<
            bcpp::concurrency::signal_tree_traits<tree_size, wrapping>>;


    template <typename Tree, typename Signal>
    bool set_signal(Tree & tree, Signal signal) noexcept
    {
        auto const result = tree.set(signal);
        return (result == Tree::set_result::set_in_empty_tree)
                || (result == Tree::set_result::set_in_non_empty_tree);
    }


    template <typename Tree, typename Hint>
    auto select_signal(Tree & tree, Hint & hint) noexcept
    {
        return tree.select(hint).signal;
    }


    template <typename Selector, typename Signals, typename Hint>
    auto select_signal(Signals & signals, Hint & hint) noexcept
    {
        auto result = signals.template select<Selector>(hint);

        if constexpr (requires { result.signal; })
            return result.signal;
        else
            return result;
    }


    inline void require(bool condition, std::string_view message)
    {
        if (not condition)
            throw std::runtime_error(std::string{message});
    }


    template <typename Function>
    void run(std::string_view name, Function && function)
    {
        function();
        std::cout << "[pass] " << name << '\n';
    }


    template <typename Function>
    int run_suite(Function && function)
    {
        try
        {
            function();
            return 0;
        }
        catch (std::exception const & exception)
        {
            std::cerr << "[fail] " << exception.what() << '\n';
            return 1;
        }
    }

} // namespace test_support
