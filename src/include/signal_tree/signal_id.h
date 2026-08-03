#pragma once

#include "./node_traits.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>


namespace bcpp
{

    enum class synchronization_mode : std::uint32_t;
    template <std::size_t tree_depth, synchronization_mode mode> class signal_tree;
    template <std::size_t tree_depth, synchronization_mode mode> class signal_set;


    //==============================================================================
    class signal_id
    {
    public:

        using value_type = std::uint64_t;

        constexpr signal_id() noexcept = default;
        constexpr explicit signal_id(value_type value) noexcept : value_(value) {}

        constexpr bool valid() const noexcept { return value_ != std::numeric_limits<value_type>::max(); }
        constexpr explicit operator value_type() const noexcept { return value_; }
        constexpr auto operator <=>(signal_id const &) const noexcept = default;

        static constexpr signal_id invalid() noexcept { return {}; }

    private:

        template <std::size_t N>
        constexpr std::size_t lane() const noexcept
        {
            using traits = node_traits<N>;
            constexpr auto mask = (value_type{1} << traits::hint_width) - value_type{1};
            return (value_ >> traits::hint_offset) & mask;
        }

        template <std::size_t N>
        constexpr signal_id with_lane(std::size_t lane) const noexcept
        {
            using traits = node_traits<N>;
            constexpr auto mask = traits::level_mask;
            return signal_id{(value_ & ~mask) | (lane << traits::hint_offset)};
        }

        value_type value_ = std::numeric_limits<value_type>::max();

        template <std::size_t, synchronization_mode> friend class signal_tree;
        template <std::size_t, synchronization_mode> friend class signal_set;
    };

} // namespace bcpp


//==============================================================================
template <>
struct std::hash<bcpp::signal_id>
{
    std::size_t operator()(bcpp::signal_id id) const noexcept
    {
        return std::hash<bcpp::signal_id::value_type>{}(static_cast<bcpp::signal_id::value_type>(id));
    }
};
