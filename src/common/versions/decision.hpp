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
        bool request_present{}, prepare{}, stable_only{}, explicit_request{};
        // Matching durable confirmation, in addition to the selected folder's startup check.
        std::set<std::string, std::less<>> confirmed_folders, published_folders;
        unsigned launches{};
        std::string last_reason;
        // Populated when both pointer records are lost, in confirmation order.
        std::vector<confirmation_entry> recovery_history;
        // Filled only after the candidate has been fully checked.
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
    int compare_slots(std::string_view a, std::string_view b);
    std::string previous_for_dialog(const disk_view& view, std::string_view attempted);
}
