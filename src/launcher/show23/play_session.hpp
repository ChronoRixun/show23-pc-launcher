#pragma once

#include "resolution.hpp"

#include <array>
#include <chrono>
#include <functional>

namespace show23
{
    enum class play_phase { idle, checking, preparing, certificates, server, playing, stopping };

    struct play_result
    {
        const char* message{};
        std::uint32_t number{};
        std::string log;
    };

    struct play_request
    {
        fs::path install;
        fs::path base;
        fs::path python;
        fs::path server;
        fs::path engine;
        bool fullscreen{};
        std::string player_name;
        resolution display_resolution = default_resolution;
    };

    struct play_options
    {
        std::array<std::uint16_t, 2> ports{10000, 10080};
        std::chrono::milliseconds ready_timeout{std::chrono::seconds(60)};
        std::function<bool()> stop;
        std::function<void(play_phase)> phase;
    };

    bool local_server_ports_available();
    const char* phase_name(play_phase phase);
    play_result run_play(const play_request& request, const play_options& options);
}
