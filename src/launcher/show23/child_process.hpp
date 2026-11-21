#pragma once

#include "input_paths.hpp"

#include <string_view>

namespace show23
{
    std::wstring quote_process_argument(std::wstring_view argument);

    class child_process
    {
    public:
        bool start(const fs::path& executable, const fs::path& directory, std::span<const std::wstring> arguments,
                   HANDLE input, HANDLE output, bool console_hidden);
        bool running() const;
        DWORD id() const;
        std::optional<DWORD> exit_code() const;
        bool stop() noexcept;

    private:
        input_paths::handle process_;
        input_paths::handle job_;
    };
}
