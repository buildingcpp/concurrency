#pragma once

#include <cstdint>


namespace bcpp
{

    // TODO: type rich
    using signal_index = std::uint64_t;

    static signal_index constexpr invalid_signal_index{~0ull};

} // namespace bcpp