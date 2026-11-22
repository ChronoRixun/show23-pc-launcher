#include "std_include.hpp"
#include "versioned_start.hpp"
#include "part_store.hpp"
#include "launcher_update.hpp"
#include "show23/game_record.hpp"

#include <utils/properties.hpp>
#include <utils/io.hpp>

namespace parts
{
    installed_runtime installed()
    {
        const auto install = show23::install_folder();
        auto result = install ? read_runtime(*install) : installed_runtime{};

        const auto& layout = versioned_start::layout();
        const auto launcher = (layout ? layout.release / "manifests" :
            utils::properties::get_appdata_path() / "data" / "launcher-release") / "launcher.json";
        std::string local_launcher;
        std::error_code error;
        if (ordinary_path(launcher) && std::filesystem::file_size(launcher, error) <= 256 * 1024 && !error)
            utils::io::read_file(launcher, &local_launcher);
        std::string parse_error;
        if (!local_launcher.empty())
        {
            auto manifest = parse(local_launcher, parse_error);
            if (manifest && manifest->part == id::launcher && manifest->version == launcher_update::running_version())
                result.manifests[index(id::launcher)] = std::move(manifest);
        }

        return result;
    }

    bool ready(const installed_runtime& runtime)
    {
        const auto install = show23::install_folder();
        return install && runtime_ready(runtime) && show23::inspect_game(*install).state == show23::record_state::ready;
    }

    bool runtime_ready(const installed_runtime& runtime)
    {
        if (runtime.root.empty() || !runtime.error.empty() ||
            !compatibility_error(runtime.manifests, launcher_update::running_version()).empty()) return false;
        std::error_code ec;
        for (const auto part : all)
        {
            if (part != id::launcher && (!runtime.manifests[index(part)] || (!ordinary_path(component_folder(runtime, part)) || !std::filesystem::is_directory(component_folder(runtime, part), ec)))) return false;
        }
        return true;
    }

    std::string verify_files(const installed_runtime& runtime)
    {
        std::string message;
        verify_runtime(runtime, launcher_update::running_version(), message);
        return message;
    }
}
