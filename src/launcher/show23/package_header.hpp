#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace show23
{
    enum class package_error { none, read, magic, truncated, layout, entry_count, metadata_missing, metadata_encrypted, metadata_size };
    struct package_header
    {
        std::string content_id;
        std::uint32_t entry_count{};
        std::uint32_t entry_offset{};
        std::uint64_t body_offset{};
        std::uint64_t body_size{};
        std::uint32_t drm_type{};
        std::uint32_t content_type{};
        std::uint32_t content_flags{};
        std::uint64_t image_offset{};
        std::uint64_t image_size{};
    };

    struct package_result
    {
        package_error error{};
        package_header header;
        std::string metadata;
    };

    using read_at = std::function<bool(std::uint64_t, std::span<char>)>;
    // Reads the header, bounded entry table, and one unencrypted parameter entry.
    package_result read_package(std::uint64_t size, const read_at& read);
}
