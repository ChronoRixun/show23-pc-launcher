#pragma once

#include "release_channel.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parts
{
    enum class id { launcher, emulator, server, profile };
    inline constexpr std::array all{id::launcher, id::emulator, id::server, id::profile};

    constexpr std::size_t index(id part) { return static_cast<std::size_t>(part); }
    const char* name(id part);
    const char* display_name(id part);
    bool is_version(std::string_view version);

    struct file
    {
        std::string path;
        std::uint64_t size;
        std::string sha256;
    };

    struct manifest
    {
        id part;
        std::string version;
        release_channel::channel channel;
        std::string min_launcher;
        std::vector<file> files;
        std::array<std::string, all.size()> compatibility;
    };

    using install_set = std::array<std::optional<manifest>, all.size()>;

    // Strict schema 1. Returns an error on failure. No network or file I/O.
    std::optional<manifest> parse(std::string_view json, std::string& error);

    // An absent part only satisfies "*" and the caller decides whether all parts are required.
    std::string compatibility_error(const install_set& proposed, std::string_view launcher_version);

    // Also enforces the installer minimum, selected channel and downgrade approval.
    std::string install_error(const install_set& installed, const manifest& candidate,
                              std::string_view running_launcher, release_channel::channel selected,
                              bool downgrade_confirmed);
}
