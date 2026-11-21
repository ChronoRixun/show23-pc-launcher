#include "std_include.hpp"
#include "package_header.hpp"
#include "input_bytes.hpp"
#include "sfo.hpp"

#include <array>
#include <string_view>

namespace show23
{
    package_result read_package(const std::uint64_t size, const read_at& read)
    {
        constexpr std::uint64_t row_size = 32;
        std::array<char, 0x420> raw{};
        package_result result;
        const auto fail = [&result](package_error error)
        {
            result.error = error;
            return std::move(result);
        };

        if (size < raw.size()) return fail(package_error::truncated);
        if (!read(0, raw)) return fail(package_error::read);
        const auto header = std::string_view(raw.data(), raw.size());
        const auto number = [header](std::size_t at, std::size_t width) { return input_bytes::integer(header, at, width, true); };
        const auto u32 = [&number](std::size_t at) { return static_cast<std::uint32_t>(number(at, 4)); };

        if (u32(0) != 0x7f434e54) return fail(package_error::magic);
        auto& facts = result.header;
        facts.entry_count = u32(0x10);
        facts.entry_offset = u32(0x18);
        facts.body_offset = number(0x20, 8);
        facts.body_size = number(0x28, 8);
        auto id = header.substr(0x40, 36);
        id = id.substr(0, id.find('\0'));
        facts.content_id = id;
        facts.drm_type = u32(0x70);
        facts.content_type = u32(0x74);
        facts.content_flags = u32(0x78);
        facts.image_offset = number(0x410, 8);
        facts.image_size = number(0x418, 8);

        if (facts.entry_count == 0 || facts.entry_count > 4096) return fail(package_error::entry_count);
        const auto table_size = facts.entry_count * row_size;
        if (!input_bytes::range(facts.entry_offset, table_size, size) ||
            !input_bytes::range(facts.body_offset, facts.body_size, size) ||
            !input_bytes::range(facts.image_offset, facts.image_size, size))
            return fail(package_error::truncated);
        if (facts.entry_offset < raw.size() || facts.image_size == 0 || facts.image_offset < raw.size())
            return fail(package_error::layout);

        std::string table(static_cast<std::size_t>(table_size), '\0');
        if (!read(facts.entry_offset, table)) return fail(package_error::read);
        std::uint64_t metadata_offset = 0;
        std::uint64_t metadata_length = 0;
        for (std::size_t at = 0; at < table.size(); at += row_size)
        {
            if (input_bytes::integer(table, at, 4, true) != 0x1000) continue;
            if (metadata_length != 0) return fail(package_error::layout);
            if (input_bytes::integer(table, at + 8, 4, true) & 0x80000000) return fail(package_error::metadata_encrypted);
            metadata_offset = input_bytes::integer(table, at + 16, 4, true);
            metadata_length = input_bytes::integer(table, at + 20, 4, true);
            if (!input_bytes::range(metadata_offset, metadata_length, size)) return fail(package_error::truncated);
            if (metadata_length == 0 || metadata_length > sfo_limit) return fail(package_error::metadata_size);
        }
        if (metadata_length == 0) return fail(package_error::metadata_missing);
        result.metadata.resize(static_cast<std::size_t>(metadata_length));
        if (!read(metadata_offset, result.metadata)) return fail(package_error::read);
        return result;
    }
}
