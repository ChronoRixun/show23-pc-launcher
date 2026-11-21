#include "std_include.hpp"
#include "setup.hpp"

#include <utils/io.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>

namespace show23
{
    namespace
    {
        constexpr step_result setup_pending{status::not_implemented, "Setup is not implemented in this version."};
    }

    bool is_local_path(const fs::path& path)
    {
        const auto drive = path.root_name().wstring();
        const auto value = path.wstring();
        return path.is_absolute() && drive.size() == 2 && drive[1] == L':' &&
            value.find(L':', 2) == std::wstring::npos && value.find(L'\0') == std::wstring::npos &&
            std::none_of(path.begin(), path.end(), [](const auto& component) { return component == L".." || component == L"."; }) &&
            GetDriveTypeW(path.root_path().c_str()) != DRIVE_REMOTE;
    }

    std::optional<fs::path> install_folder()
    {
        const auto saved = utils::properties::load(property_keys::INSTALL);
        if (!saved) return std::nullopt;
        const auto path = utils::string::utf8_to_path(*saved);
        return is_local_path(path) ? std::optional(path) : std::nullopt;
    }

    step_result begin_setup(const setup_request& request)
    {
        if (!is_local_path(request.base) || (request.update && !is_local_path(*request.update)) ||
            !is_local_path(request.install) || request.install == request.install.root_path())
            return {status::rejected, "Choose the game input and an install folder on a local drive."};

        for (const auto& root : {utils::properties::get_local_root(), utils::properties::get_portable_root()})
        {
            if (_wcsicmp(request.install.lexically_normal().c_str(), root.lexically_normal().c_str()) == 0 ||
                utils::io::is_inside_folder(request.install, root))
                return {status::rejected, "Choose an install folder outside the launcher's settings folder."};
        }

        // This only remembers the destination for Mods and Verify Files. The game inputs stay local
        // to this setup request and no ready marker is written.
        utils::properties::store(property_keys::INSTALL, utils::string::path_to_utf8(request.install));
        checked_inputs checked;
        return check_inputs(request, checked);
    }

    step_result check_inputs(const setup_request&, checked_inputs&) { return setup_pending; }
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
