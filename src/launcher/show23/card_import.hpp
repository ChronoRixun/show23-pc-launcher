#pragma once

#include "setup.hpp"

#include <functional>
#include <string_view>

namespace show23
{
    struct card_status
    {
        unsigned cards{};
        std::string date;
    };

    struct card_progress
    {
        unsigned page{};
        unsigned total{};
        unsigned cards{};
        bool done{};
    };

    bool parse_card_progress(std::string_view line, card_progress& progress);
    std::optional<card_status> read_card_status(const fs::path& install);

    struct card_import_result
    {
        const char* status;
        fs::path log;
    };

    class activity_lock
    {
    public:
        activity_lock();
        ~activity_lock();
        activity_lock(const activity_lock&) = delete;
        activity_lock& operator=(const activity_lock&) = delete;
        explicit operator bool() const { return held_; }

    private:
        bool held_{};
    };

    card_import_result import_cards(const fs::path& install, const fs::path& server_folder,
        const std::function<bool()>& cancel, const std::function<void(const card_progress&)>& report);
}
