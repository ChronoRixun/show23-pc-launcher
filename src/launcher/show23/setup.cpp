#include "std_include.hpp"
#include "setup.hpp"
#include "input_checks.hpp"

#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>

namespace show23
{
    namespace
    {
        constexpr step_result setup_pending{status::not_implemented, "Setup is not implemented in this version."};
    }

    std::optional<fs::path> install_folder()
    {
        const auto saved = utils::properties::load(property_keys::INSTALL);
        if (!saved) return std::nullopt;
        const auto path = utils::string::utf8_to_path(*saved);
        return is_local_path(path) ? std::optional(path) : std::nullopt;
    }

    checked_inputs begin_setup(const setup_request& request)
    {
        auto report = check_inputs(request, default_input_check_options());
        if (report.passed())
            utils::properties::store(property_keys::INSTALL, utils::string::path_to_utf8(request.install));
        return report;
    }

    step_result unpack_into_staging(const checked_inputs&, const fs::path&, staged_game&) { return setup_pending; }
    step_result verify_game(const staged_game&, const fs::path&) { return setup_pending; }
    step_result generate_settings(const checked_inputs&, const staged_game&, const fs::path&) { return setup_pending; }
    step_result adopt_install(const staged_game&, const fs::path&) { return setup_pending; }
    step_result start_local_server(const fs::path&, const fs::path&)
    {
        return {status::not_implemented, "Starting the local server is not implemented in this version."};
    }
    step_result start_emulator(const fs::path&, const fs::path&)
    {
        return {status::not_implemented, "Starting the emulator is not implemented in this version."};
    }
}
