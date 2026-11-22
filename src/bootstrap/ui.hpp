#pragma once

#include <versions/records.hpp>

namespace bootstrap
{
    void log(const versions::fs::path& root, std::string_view line);
    enum class answer { close, wait, previous, retry };
    answer dialog(const versions::fs::path& root, std::wstring title, std::wstring detail, std::string_view newest,
        bool can_wait = false, std::string_view previous = {}, bool can_retry = false, bool release_page = true);
    versions::verification check_arrival(const versions::fs::path& folder, bool quiet);
    bool print_folder(std::string_view folder);
}
