#pragma once

#include "records.hpp"

#include <map>
#include <set>

namespace versions
{
    enum class step { validate_names, process_request, arrival, choose, confirmed, retry };
    enum class action { next, scan, launch_trial, launch_current, prepared, dialog_none, dialog_write, dialog_current, wait };

    struct disk_view
    {
        pointer state;
        step where{};
        std::map<std::string, verification, std::less<>> folders;
        std::optional<request> incoming;
        bool request_present{}, prepare{};
        unsigned launches{};
        std::string last_reason;
        // Filled only after the newest quick candidate has been fully checked.
        std::string checked_arrival;
        verification arrival_result;
        std::set<std::string, std::less<>> rejected_arrivals;
    };

    struct decision
    {
        action next{};
        std::optional<pointer> save;
        std::string folder;
        bool remove_request{}, invalid_request{};
    };

    decision decide(const disk_view& view);
    decision failed_commit(const disk_view& view);
}
