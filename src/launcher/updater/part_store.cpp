#include "std_include.hpp"
#include "part_store.hpp"
#include "show23/setup.hpp"
#include "show23/input_paths.hpp"

#include <format>

namespace parts
{
    namespace fs = std::filesystem;
    namespace
    {
        // Files and folders in the install layout must remain on the selected local drive.
        std::string read_small(const fs::path& path, const std::uintmax_t limit)
        {
            const auto file = show23::input_paths::inspect(path, false, true);
            if (!file.present || file.directory || !file.length || file.length > limit ||
                file.error != show23::input_paths::path_error::none) return {};
            std::string text(static_cast<std::size_t>(file.length), '\0');
            if (!show23::input_paths::read_at(file.pins.back().get(), 0, text)) return {};
            return text;
        }

        bool matches(const fs::path& file, const parts::file& expected)
        {
            const auto input = show23::input_paths::inspect(file, false, true);
            return input.present && !input.directory && input.error == show23::input_paths::path_error::none &&
                input.length == expected.size && show23::input_paths::sha256(input) == expected.sha256;
        }

    }

    bool ordinary_path(const fs::path& path, const bool allow_missing)
    {
        if (!show23::is_local_path(path)) return false;
        auto current = path.root_path();
        for (const auto& segment : path.relative_path())
        {
            current /= segment;
            const auto attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto error = GetLastError();
                return allow_missing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
            }
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        }
        return true;
    }

    installed_runtime read_runtime(const fs::path& install)
    {
        installed_runtime result;
        const auto manifests = install / "manifests";
        const auto selection = manifests / "runtime.txt";
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(selection, ec)) && (!ec || ec == std::errc::no_such_file_or_directory))
            return result;
        auto release = read_small(selection, 128);
        while (!release.empty() && (release.back() == '\r' || release.back() == '\n')) release.pop_back();
        if (!is_version(release))
        {
            result.error = "The active runtime record is invalid.";
            return result;
        }

        return read_runtime(install, release);
    }

    installed_runtime read_runtime(const fs::path& install, const std::string& release)
    {
        installed_runtime result;
        if (!is_version(release)) return result;
        result.root = install / "runtime" / release;
        const auto manifests = install / "manifests";
        std::string parse_error;
        for (const auto part : all)
        {
            if (part == id::launcher) continue;
            const auto json = read_small(manifests / release / (std::string(name(part)) + ".json"), 256 * 1024);
            auto item = parse(json, parse_error);
            if (!item || item->part != part)
                result.error = "The active runtime has a missing or invalid part manifest.";
            else
                result.manifests[index(part)] = std::move(item);
        }
        return result;
    }

    fs::path component_folder(const installed_runtime& runtime, const id part)
    {
        if (part == id::profile) return runtime.root / "tools" / "profile";
        return runtime.root / name(part);
    }

    bool verify_card_import_runtime(const installed_runtime& runtime, const std::string_view launcher_version, std::string& message)
    {
        if (!verify_runtime(runtime, launcher_version, message)) return false;
        const auto& files = runtime.manifests[index(id::server)]->files;
        for (const auto* required : {"python/python.exe", "olserver.py"})
        {
            if (std::ranges::any_of(files, [&](const auto& file) { return file.path == required; })) continue;
            message = "The server manifest does not cover the card importer.";
            return false;
        }
        return true;
    }

    bool verify_runtime(const installed_runtime& runtime, const std::string_view launcher_version, std::string& message)
    {
        if (!runtime.error.empty()) { message = runtime.error; return false; }
        if (runtime.root.empty()) { message = "No active runtime to verify."; return false; }
        message = compatibility_error(runtime.manifests, launcher_version);
        if (!message.empty()) return false;

        std::size_t checked = 0;
        std::size_t failed = 0;
        std::string first;
        for (const auto part : all)
        {
            if (part == id::launcher) continue;
            const auto& item = runtime.manifests[index(part)];
            if (!item) { message = "A runtime manifest is missing."; return false; }
            const auto folder = component_folder(runtime, part);
            for (const auto& file : item->files)
            {
                ++checked;
                if (matches(folder / file.path, file)) continue;
                if (first.empty()) first = std::string(name(part)) + "/" + file.path;
                ++failed;
            }
        }
        message = failed ? std::format("{} of {} runtime files failed verification (first: {}).", failed, checked, first) :
            std::format("Verified {} runtime files.", checked);
        return !failed;
    }

}
