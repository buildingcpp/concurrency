#pragma once

#include <chrono>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>


namespace wc_test
{

    class suite
    {
    public:
        explicit suite(std::string_view name) : name_(name)
        {
            std::cout << name_ << "\n";
        }

        void check(bool condition, std::string_view description)
        {
            std::cout << (condition ? "  ok:   " : "  FAIL: ") << description << "\n";
            failures_ += condition ? 0 : 1;
        }

        template <typename Expected, typename Function>
        void throws(Function && function, std::string_view description)
        {
            try
            {
                function();
                check(false, description);
            }
            catch (Expected const &)
            {
                check(true, description);
            }
            catch (...)
            {
                check(false, description);
            }
        }

        int finish() const
        {
            std::cout << "\n"
                      << (failures_ == 0 ? "all passed, failures = " : "FAILURES: ")
                      << failures_ << "\n";
            return failures_ != 0;
        }

    private:
        std::string_view    name_;
        int                 failures_{};
    };


    template <typename Predicate, typename Rep, typename Period>
    bool eventually(Predicate && predicate, std::chrono::duration<Rep, Period> timeout)
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while (not predicate())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }


    template <typename Group>
    std::size_t drain(Group & group, std::size_t limit = 100'000)
    {
        std::size_t executed{};
        while (executed < limit && group.execute_next_contract())
            ++executed;
        return executed;
    }

} // namespace wc_test
