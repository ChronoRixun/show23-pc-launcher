#pragma once

#include "release_channel.hpp"

#include <rapidjson/document.h>

#include <string>

namespace launcher_update
{
    // Startup and shutdown with the launcher single-instance lock held.
    void cleanup();
    void stop();

    void check();
    // Only a newer release, or the exact older Stable release approved by the player.
    bool start_download(const std::string& downgrade_to = {});
    bool retry(const std::string& folder);
    bool restart(std::string& error);

    void write_status(rapidjson::Value& out, rapidjson::Document::AllocatorType& allocator);
    std::string release_page();
    std::string installed_version();
    std::string running_version();

    // The channel setting and the update selection change together.
    bool channel_changed(release_channel::channel selected);
    bool revoke_downgrade();
}
