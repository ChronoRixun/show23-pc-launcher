#pragma once

#include "input_paths.hpp"

#include <functional>

namespace show23
{
    struct unpacker_program
    {
        input_paths::location file;
        bool bundled{};
        bool subfolder{};
        std::string consent;
        const char* error{};
    };

    unpacker_program find_unpacker(const fs::path& selected, const fs::path& bundled);
    std::wstring quote_process_argument(std::wstring_view argument);

    struct extraction_result
    {
        fs::path folder;
        const char* error{};
        std::uint32_t exit_code{};
    };

    extraction_result extract_package(const unpacker_program& program, const fs::path& package,
        const fs::path& output, const fs::path& log, const std::function<bool()>& cancel,
        const std::function<void(std::uint64_t elapsed, std::uint64_t written)>& progress);
}
