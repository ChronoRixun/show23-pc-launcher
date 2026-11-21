#include "std_include.hpp"
#include "part_store.hpp"
#include "launcher_update.hpp"
#include "show23/setup.hpp"
#include "show23/game_version.hpp"

#include <utils/cryptography.hpp>
#include <utils/properties.hpp>

namespace parts
{
    namespace fs = std::filesystem;
    namespace
    {
        // Files and folders in the install layout must remain on the selected local drive.
        std::string read_small(const fs::path& path, const std::uintmax_t limit)
        {
            std::error_code ec;
            if (!ordinary_path(path) || !fs::is_regular_file(path, ec)) return {};
            const auto size = fs::file_size(path, ec);
            if (ec || size == 0 || size > limit) return {};
            std::string text(static_cast<std::size_t>(size), '\0');
            std::ifstream stream(path, std::ios::binary);
            if (!stream.read(text.data(), static_cast<std::streamsize>(text.size())) || stream.peek() != std::char_traits<char>::eof())
                return {};
            return text;
        }

        bool directory(const fs::path& path)
        {
            std::error_code ec;
            return ordinary_path(path) && fs::is_directory(path, ec);
        }

        bool matches(const fs::path& file, const parts::file& expected)
        {
            std::error_code ec;
            if (!ordinary_path(file) || !fs::is_regular_file(file, ec) ||
                fs::file_size(file, ec) != expected.size || ec) return false;
            std::ifstream stream(file, std::ios::binary);
            if (!stream) return false;
            utils::cryptography::sha256::stream hash;
            std::array<char, 64 * 1024> buffer;
            std::uint64_t remaining = expected.size;
            while (remaining)
            {
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
                if (!stream.read(buffer.data(), count)) return false;
                hash.update(buffer.data(), static_cast<std::size_t>(count));
                remaining -= static_cast<std::uint64_t>(count);
            }
            return stream.peek() == std::char_traits<char>::eof() && !stream.bad() && hash.finish_hex() == expected.sha256;
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

    installed_runtime installed()
    {
        installed_runtime result;
        const auto launcher = utils::properties::get_appdata_path() / "data" / "launcher-release" / "launcher.json";
        const auto local_launcher = read_small(launcher, 256 * 1024);
        std::string parse_error;
        if (!local_launcher.empty())
        {
            auto manifest = parse(local_launcher, parse_error);
            if (manifest && manifest->part == id::launcher && manifest->version == launcher_update::running_version())
                result.manifests[index(id::launcher)] = std::move(manifest);
        }

        const auto install = show23::install_folder();
        if (!install) return result;
        const auto manifests = *install / "manifests";
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

        result.root = *install / "runtime" / release;
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

    bool ready(const installed_runtime& runtime)
    {
        const auto install = show23::install_folder();
        if (!install || runtime.root.empty() || !runtime.error.empty() ||
            !compatibility_error(runtime.manifests, launcher_update::running_version()).empty()) return false;
        const auto game = *install / "data" / "games" / show23::title_id;
        if (!directory(game / "base")) return false;
        auto marker = read_small(*install / "manifests" / "game-ready.txt", 32);
        while (!marker.empty() && (marker.back() == '\r' || marker.back() == '\n')) marker.pop_back();
        if (marker != show23::title_id) return false;
        for (const auto part : all)
        {
            if (part != id::launcher && (!runtime.manifests[index(part)] || !directory(component_folder(runtime, part)))) return false;
        }
        return true;
    }

    game_display read_game_display(const fs::path& install)
    {
        const auto game = install / "data" / "games" / show23::title_id;
        const auto has_update = directory(game / "update");
        const auto source = game / (has_update ? "update" : "base") / "sce_sys" / "param.sfo";
        return {show23::parse_app_version(read_small(source, 64 * 1024)), has_update};
    }

    std::string verify_files(const installed_runtime& runtime)
    {
        if (!runtime.error.empty()) return runtime.error;
        if (runtime.root.empty()) return "No active runtime to verify. Set up the game first.";
        const auto compatibility = compatibility_error(runtime.manifests, launcher_update::running_version());
        if (!compatibility.empty()) return compatibility;

        std::size_t checked = 0;
        std::size_t failed = 0;
        std::string first;
        for (const auto part : all)
        {
            if (part == id::launcher) continue;
            const auto& item = runtime.manifests[index(part)];
            if (!item) return "A runtime manifest is missing.";
            const auto folder = component_folder(runtime, part);
            for (const auto& file : item->files)
            {
                ++checked;
                if (matches(folder / file.path, file)) continue;
                if (first.empty()) first = std::string(name(part)) + "/" + file.path;
                ++failed;
            }
        }
        if (failed) return std::format("{} of {} runtime files failed verification (first: {}). Repair is not implemented in this version.",
                                       failed, checked, first);
        return std::format("Verified {} runtime files. Game file verification is not implemented in this version.", checked);
    }
}
