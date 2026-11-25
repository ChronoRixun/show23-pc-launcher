#pragma once

#include "input_paths.hpp"

#include <string_view>
#include <utility>

namespace show23
{
    std::wstring quote_process_argument(std::wstring_view argument);
    using environment_entry = std::pair<std::wstring, std::wstring>;

    class child_process
    {
    public:
        bool start(const fs::path& executable, const fs::path& directory, std::span<const std::wstring> arguments,
                   HANDLE input, HANDLE output, bool console_hidden,
                   std::span<const environment_entry> overrides = {}, std::wstring_view ignored_prefix = {});
        bool running() const;
        DWORD id() const;
        std::optional<DWORD> exit_code() const;
        bool stop() noexcept;

    private:
        input_paths::handle process_;
        input_paths::handle job_;
    };
}
