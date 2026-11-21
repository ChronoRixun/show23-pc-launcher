#pragma once

#include "setup.hpp"

#include <functional>
#include <string_view>

namespace show23
{
    struct executable_reference
    {
        std::uint64_t size;
        std::string_view sha256;
    };

    struct input_check_options
    {
        executable_reference base{38'785'273, "78858c91ee74e69056c8000cb54d3451b2d738b9b9d3d89c006b704be3ec3aa1"};
        executable_reference update{39'247'689, "ceb28e9b8e96e582214e785682f2d4f27dea0746edb4b4c6a36df62dc2bd9bec"};
        std::function<std::optional<std::uint64_t>(const fs::path&)> free_space;
        std::function<bool(const fs::path&, bool creating_folder)> writable;
        std::vector<fs::path> protected_destinations;
    };

    // Console and window use the same checker. This function does not save any settings.
    input_check_options default_input_check_options();
    checked_inputs check_inputs(const setup_request& request, const input_check_options& options);
}
