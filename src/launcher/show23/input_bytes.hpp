#pragma once

#include <cstdint>
#include <string_view>

namespace show23::input_bytes
{
    constexpr bool range(const std::uint64_t offset, const std::uint64_t size, const std::uint64_t total)
    {
        return offset <= total && size <= total - offset;
    }

    // Call only on a range already validated against the source buffer.
    constexpr std::uint64_t integer(const std::string_view bytes, const std::size_t at,
                                    const std::size_t width, const bool big_endian = false)
    {
        std::uint64_t result = 0;
        for (std::size_t n = 0; n < width; ++n)
        {
            const auto shift = (big_endian ? width - n - 1 : n) * 8;
            result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at + n])) << shift;
        }
        return result;
    }
}
