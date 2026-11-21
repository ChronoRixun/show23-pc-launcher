#include "std_include.hpp"
#include "setup.hpp"
#include "input_paths.hpp"

#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>

namespace show23
{
    std::optional<fs::path> install_folder()
    {
        const auto saved = utils::properties::load(property_keys::INSTALL);
        if (!saved) return std::nullopt;
        const auto path = utils::string::utf8_to_path(*saved);
        return is_local_path(path) ? std::optional(path) : std::nullopt;
    }

    bool remember_install(const fs::path& install)
    {
        const auto properties = input_paths::inspect(utils::properties::get_appdata_path() / "user" / "properties.json", true);
        if (properties.error != input_paths::path_error::none || properties.directory || properties.links > 1) return false;
        const auto path = utils::string::path_to_utf8(install);
        utils::properties::store(property_keys::INSTALL, path);
        return utils::properties::load(property_keys::INSTALL) == path;
    }

}
