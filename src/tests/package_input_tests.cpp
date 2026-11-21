#include "std_include.hpp"
#include "test.hpp"
#include "input_fixtures.hpp"
#include "show23/package_header.hpp"
#include "show23/sfo.hpp"

namespace
{
    show23::package_result read(std::string_view bytes)
    {
        return show23::read_package(bytes.size(), [bytes](std::uint64_t offset, std::span<char> output)
        {
            if (offset > bytes.size() || output.size() > bytes.size() - offset) return false;
            std::copy_n(bytes.data() + static_cast<std::size_t>(offset), output.size(), output.data());
            return true;
        });
    }
}

TEST(package_reader_loads_only_the_synthetic_header_table_and_parameter_entry)
{
    const auto result = read(input_fixtures::package());
    CHECK(result.error == show23::package_error::none);
    CHECK(result.header.content_id == input_fixtures::content);
    CHECK(result.header.content_type == 0x1a);
    CHECK(result.header.image_size == 17);
    CHECK(result.metadata == input_fixtures::sfo());
}

TEST(package_reader_reports_bad_magic_and_multiple_prefix_truncations)
{
    using show23::package_error;
    auto bytes = input_fixtures::package();
    for (const auto length : {std::size_t{2}, std::size_t{0x417}, std::size_t{0x448}, bytes.size() - 1})
        CHECK(read(std::string_view(bytes).substr(0, length)).error == package_error::truncated);
    bytes[0] = '\0';
    CHECK(read(bytes).error == package_error::magic);
}

TEST(package_reader_rejects_out_of_bounds_ranges_and_entry_count)
{
    using show23::package_error;
    for (const auto offset : {0x18, 0x20, 0x410})
    {
        auto bytes = input_fixtures::package();
        input_fixtures::put(bytes, offset, bytes.size() + 1, offset == 0x18 ? 4 : 8, true);
        CHECK(read(bytes).error == package_error::truncated);
    }
    auto bytes = input_fixtures::package();
    input_fixtures::put(bytes, 0x10, 4097, 4, true);
    CHECK(read(bytes).error == package_error::entry_count);
    bytes = input_fixtures::package();
    input_fixtures::put(bytes, 0x418, UINT64_MAX, 8, true);
    CHECK(read(bytes).error == package_error::truncated);
}

TEST(package_reader_rejects_missing_encrypted_or_out_of_bounds_parameters)
{
    using show23::package_error;
    auto bytes = input_fixtures::package();
    input_fixtures::put(bytes, 0x440, 0x2000, 4, true);
    CHECK(read(bytes).error == package_error::metadata_missing);
    bytes = input_fixtures::package();
    input_fixtures::put(bytes, 0x448, 0x80000000, 4, true);
    CHECK(read(bytes).error == package_error::metadata_encrypted);
    bytes = input_fixtures::package();
    input_fixtures::put(bytes, 0x450, bytes.size(), 4, true);
    CHECK(read(bytes).error == package_error::truncated);
}

TEST(parameter_reader_checks_offsets_lengths_and_the_text_formats)
{
    auto bytes = input_fixtures::sfo();
    auto doc = show23::read_sfo(bytes);
    CHECK(doc && doc->title == "CUSA35521" && doc->version == "01.00" && doc->content_id == input_fixtures::content);

    input_fixtures::put(bytes, 22, 0x0004, 2);
    input_fixtures::put(bytes, 24, 9, 4);
    doc = show23::read_sfo(bytes);
    CHECK(doc && doc->title == "CUSA35521");

    input_fixtures::put(bytes, 8, 4, 4);
    CHECK(!show23::read_sfo(bytes));
    bytes = input_fixtures::sfo();
    input_fixtures::put(bytes, 16, 4097, 4);
    CHECK(!show23::read_sfo(bytes));
    bytes = input_fixtures::sfo(false, std::string(300, 'a'));
    CHECK(!show23::read_sfo(bytes));
    bytes = input_fixtures::sfo();
    input_fixtures::put(bytes, 32, UINT32_MAX, 4);
    CHECK(!show23::read_sfo(bytes));
}

TEST(parameter_reader_distinguishes_a_missing_key_and_rejects_a_duplicate)
{
    auto bytes = input_fixtures::sfo();
    bytes[84] = 'X'; // rename TITLE_ID in the key table
    const auto doc = show23::read_sfo(bytes);
    CHECK(doc && doc->title.empty() && doc->version == "01.00");

    bytes = input_fixtures::sfo();
    input_fixtures::put(bytes, 36, 0, 2); // category entry points at TITLE_ID
    CHECK(!show23::read_sfo(bytes));
}
