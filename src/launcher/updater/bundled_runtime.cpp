#include "std_include.hpp"
#include "part_store.hpp"
#include "show23/input_paths.hpp"

#include <utils/finally.hpp>
#include <atomic>
#include <utility>

namespace parts
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace show23::input_paths;

        bool move(const fs::path& from, const fs::path& to)
        {
            return MoveFileExW(extended(from).c_str(), extended(to).c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
        }

        bool complete_layout(const installed_runtime& runtime)
        {
            const auto listed = [&](id part, std::string_view path)
            {
                const auto& item = runtime.manifests[index(part)];
                return item && std::ranges::any_of(item->files, [&](const auto& file) { return file.path == path; });
            };
            return listed(id::emulator, "shadPS4.exe") && listed(id::server, "python/python.exe") &&
                listed(id::server, "olserver.py") && listed(id::server, "roster_decode.py") &&
                listed(id::server, "card_import.py") && listed(id::server, "tables/ol-messages.csv") &&
                std::ranges::any_of(runtime.manifests[index(id::server)]->files, [](const auto& file)
                {
                    return file.path.starts_with("replies/");
                });
        }

        bool copy_file_checked(const fs::path& source, const fs::path& target)
        {
            const auto input = inspect(source, false, true);
            if (!input.present || input.error != path_error::none || input.directory) return false;
            const auto parent = ensure_directory(target.parent_path());
            if (!parent.directory || parent.error != path_error::none) return false;
            return CopyFileW(extended(input.path).c_str(), extended(parent.path / target.filename()).c_str(), TRUE) != FALSE;
        }
    }

    bool stage_bundled_runtime(const fs::path& bundle, const fs::path& install, const std::string_view launcher_version)
    {
        std::string verification;
        const auto active = read_runtime(install);
        if (complete_layout(active) && verify_runtime(active, launcher_version, verification)) return true;
        if (show23::input_paths::contains(bundle, install) || show23::input_paths::contains(install, bundle)) return false;

        const auto source = read_runtime(bundle);
        if (!complete_layout(source) || !verify_runtime(source, launcher_version, verification)) return false;
        const auto release = source.root.filename().string();
        for (const auto part : {id::emulator, id::server, id::profile})
            if (source.manifests[index(part)]->version != release) return false;
        const auto manifest_root = ensure_directory(install / "manifests");
        const auto runtime_root = ensure_directory(install / "runtime");
        if (!manifest_root.directory || !runtime_root.directory || manifest_root.error != path_error::none ||
            runtime_root.error != path_error::none) return false;

        const auto existing = read_runtime(install, release);
        if (complete_layout(existing) && verify_runtime(existing, launcher_version, verification))
            return atomically_write(manifest_root, "runtime.txt", release + '\n');

        // Publish the active marker only after both the files and manifests verify in staging.
        static std::atomic_uint serial{};
        const auto temp = runtime_root.path / (".bundled-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(++serial));
        if (!CreateDirectoryW(extended(temp).c_str(), nullptr)) return false;
        bool preserve_backup = false;
        const auto clean = utils::finally([&]
        {
            if (preserve_backup) return;
            std::error_code ignored;
            fs::remove_all(extended(temp), ignored);
        });
        const auto stage = temp / "new";
        installed_runtime staged{stage / "runtime" / release, source.manifests, {}};
        for (const auto part : {id::emulator, id::server, id::profile})
        {
            for (const auto& file : source.manifests[index(part)]->files)
                if (!copy_file_checked(component_folder(source, part) / file.path,
                                       component_folder(staged, part) / file.path)) return false;
            const auto name = std::string(parts::name(part)) + ".json";
            if (!copy_file_checked(bundle / "manifests" / release / name, stage / "manifests" / release / name)) return false;
        }
        if (!verify_runtime(read_runtime(stage, release), launcher_version, verification)) return false;

        const auto publish = [&](const fs::path& from, const fs::path& to, const fs::path& backup, bool& saved)
        {
            bool present{};
            {
                const auto old = inspect(to, true);
                if (old.error != path_error::none || (old.present && (!old.directory || !folder_size(old.path)))) return false;
                present = old.present;
            }
            if (present && !move(to, backup)) return false;
            saved = present;
            if (move(from, to)) return true;
            if (saved && !move(backup, to)) preserve_backup = true;
            return false;
        };
        const auto restore = [&](const fs::path& from, const fs::path& to, const fs::path& backup, bool saved)
        {
            if (!move(to, from) || (saved && !move(backup, to))) preserve_backup = true;
        };
        const auto runtime = runtime_root.path / release;
        const auto manifests = manifest_root.path / release;
        bool runtime_saved{}, manifests_saved{};
        if (!publish(staged.root, runtime, temp / "old-runtime", runtime_saved)) return false;
        if (!publish(stage / "manifests" / release, manifests, temp / "old-manifests", manifests_saved))
        {
            restore(staged.root, runtime, temp / "old-runtime", runtime_saved);
            return false;
        }
        if (atomically_write(manifest_root, "runtime.txt", release + '\n')) return true;
        restore(stage / "manifests" / release, manifests, temp / "old-manifests", manifests_saved);
        restore(staged.root, runtime, temp / "old-runtime", runtime_saved);
        return false;
    }
}
