#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace versioned_start
{
    struct paths
    {
        std::filesystem::path release, versions, bootstrap;
        std::string folder;
        explicit operator bool() const { return !bootstrap.empty(); }
    };

    paths locate(const std::filesystem::path& executable);
    void initialize(const std::filesystem::path& executable);
    const paths& layout();
    bool accept_parent();
    bool hand_over(bool online = false);
    bool confirm();
    enum class gate { waiting, open, deferred };
    gate status();
    bool unlocked();
    bool command_allowed(std::string_view command);
    std::string take_notices();
    bool request_restart();
    bool restarting();
    void set_taskbar_target(void* window);
}
