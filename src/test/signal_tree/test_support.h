#pragma once

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>


namespace test_support
{

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
