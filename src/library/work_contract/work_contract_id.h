#pragma once

#include <library/signal_tree/signal_id.h>

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>


namespace bcpp::concurrency
{

    //==============================================================================
    // identifies a contract within its owning work_contract_group.  the value is
    // the signal index within that group's signal_set so conversion is free.
    class work_contract_id
    {
    public:

        using value_type = std::uint64_t;

        constexpr work_contract_id() noexcept = default;

        constexpr explicit work_contract_id(value_type value) noexcept : value_(value) {}

        constexpr explicit work_contract_id(signal_id id) noexcept :
            value_(static_cast<value_type>(id)) {}

        constexpr bool valid() const noexcept { return value_ != invalid_value; }

        constexpr explicit operator value_type() const noexcept { return value_; }

        constexpr signal_id to_signal_id() const noexcept { return signal_id{value_}; }

        constexpr auto operator <=>(work_contract_id const &) const noexcept = default;

        static constexpr work_contract_id invalid() noexcept { return {}; }

    private:

        static constexpr value_type invalid_value = std::numeric_limits<value_type>::max();

        value_type value_{invalid_value};

    }; // class work_contract_id

} // namespace bcpp::concurrency


//==============================================================================
template <>
struct std::hash<bcpp::concurrency::work_contract_id>
{
    std::size_t operator()(bcpp::concurrency::work_contract_id id) const noexcept
    {
        return std::hash<bcpp::concurrency::work_contract_id::value_type>{}(static_cast<bcpp::concurrency::work_contract_id::value_type>(id));
    }
};
