#include "std_include.hpp"
#include "game_mods.hpp"
#include "tools/archive.hpp"
#include "updater/part_store.hpp"

#include <utils/finally.hpp>
#include <utils/io.hpp>
#include <utils/string.hpp>

#include <rapidjson/document.h>

#include <format>
#include <unordered_set>
#include <random>

namespace game_mods
{
    namespace
    {
        // Serialises changes to one game's mods folder (UI commands and the import thread).
        std::mutex mutex_;

        constexpr auto load_order_name = L"load-order.txt";
        constexpr auto staging_prefix = ".staging-";

        struct order_entry
        {
            std::string name;
            bool enabled{};
        };

        std::string lower(const std::string& text)
        {
            return utils::string::to_lower(text);
        }

        bool is_hidden(const std::string& name)
        {
            return name.empty() || name.front() == '.';
        }

        // Folder names Windows accepts, minus leading dots (reserved for staging).
        std::string sanitize_name(std::string name)
        {
            std::string result;
            for (const char c : name)
            {
                if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
                {
                    continue;
                }
                result += c;
            }
            while (!result.empty() && (result.back() == '.' || result.back() == ' '))
            {
                result.pop_back();
            }
            while (!result.empty() && (result.front() == '.' || result.front() == ' '))
            {
                result.erase(result.begin());
            }
            if (result.size() > 100)
            {
                result.resize(100);
            }
            return result;
        }

        std::vector<order_entry> read_order(const std::filesystem::path& folder)
        {
            std::vector<order_entry> entries;
            std::string text;
            if (!parts::ordinary_path(folder / load_order_name) || !utils::io::read_file(folder / load_order_name, &text))
            {
                return entries;
            }
            if (text.starts_with("\xEF\xBB\xBF"))
            {
                text.erase(0, 3);
            }

            for (auto line : utils::string::split(text, '\n'))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                {
                    line.pop_back();
                }
                if (line.size() < 2 || (line.front() != '+' && line.front() != '-'))
                {
                    continue;
                }

                const auto name = line.substr(1);
                std::erase_if(entries, [&](const order_entry& e) { return lower(e.name) == lower(name); });
                entries.push_back({name, line.front() == '+'});
            }
            return entries;
        }

        bool write_order(const std::filesystem::path& folder, const std::vector<order_entry>& entries)
        {
            std::string text =
                "# Content pack order, managed by show23-pc launcher.\r\n"
                "# +Name = enabled, -Name = disabled.\r\n";
            for (const auto& entry : entries)
            {
                text += (entry.enabled ? "+" : "-") + entry.name + "\r\n";
            }

            const auto target = folder / load_order_name;
            const auto temp = folder / L"load-order.txt.tmp";
            if (!parts::ordinary_path(temp, true) || !parts::ordinary_path(target, true)) return false;
        return utils::io::write_file(temp, text) && MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }

        bool has_pack_manifest(const std::filesystem::path& folder)
        {
            const auto file = folder / L"pack.json";
            const auto attributes = GetFileAttributesW(file.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
        }

        std::vector<std::string> mod_folders(const std::filesystem::path& folder)
        {
            std::vector<std::string> names;
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
            {
                const auto name = utils::string::path_to_utf8(entry.path().filename());
                if (entry.is_directory(error) && parts::ordinary_path(entry.path()) && !is_hidden(name) && has_pack_manifest(entry.path()))
                {
                    names.push_back(name);
                }
            }
            std::ranges::sort(names, [](const auto& a, const auto& b) { return lower(a) < lower(b); });
            return names;
        }

        // Brings the load order in line with the folders: drops missing mods, appends new ones.
        std::vector<order_entry> reconcile(const std::filesystem::path& folder)
        {
            auto entries = read_order(folder);
            const auto folders = mod_folders(folder);
            const auto before = entries.size();

            std::erase_if(entries, [&](const order_entry& entry)
            {
                return std::ranges::none_of(folders, [&](const auto& name) { return lower(name) == lower(entry.name); });
            });
            auto changed = entries.size() != before;

            for (const auto& name : folders)
            {
                const auto listed = std::ranges::any_of(entries, [&](const auto& entry) { return lower(entry.name) == lower(name); });
                if (!listed)
                {
                    entries.push_back({name, true});
                    changed = true;
                }
            }

            if (changed || !utils::io::file_exists(folder / load_order_name))
            {
                write_order(folder, entries);
            }
            return entries;
        }

        void read_metadata(const std::filesystem::path& folder, mod_info& info)
        {
            std::string text;
            if (!utils::io::read_file(folder / L"pack.json", &text))
            {
                return;
            }

            rapidjson::Document document;
            document.Parse(text.data(), text.size());
            if (document.HasParseError() || !document.IsObject())
            {
                return;
            }

            const auto get = [&](const char* key) -> std::string
            {
                const auto member = document.FindMember(key);
                return member != document.MemberEnd() && member->value.IsString() ? member->value.GetString() : std::string{};
            };
            info.title = get("name");
            info.version = get("version");
            info.author = get("author");
            info.description = get("description");
        }

        mod_info describe(const std::filesystem::path& folder, const order_entry& entry)
        {
            mod_info info{entry.name, entry.enabled};
            std::error_code error;
            for (auto it = std::filesystem::recursive_directory_iterator(folder / utils::string::utf8_to_path(entry.name), error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
            {
                if (it->is_regular_file(error))
                {
                    ++info.files;
                    info.size += it->file_size(error);
                }
            }
            read_metadata(folder / utils::string::utf8_to_path(entry.name), info);
            return info;
        }

        // Accept a pack at the root or in one top-level wrapper folder.
        std::optional<std::filesystem::path> find_content_root(const std::filesystem::path& staging)
        {
            if (has_pack_manifest(staging)) return staging;
            std::optional<std::filesystem::path> root;
            for (const auto& entry : std::filesystem::directory_iterator(staging))
            {
                if (!entry.is_directory() || !has_pack_manifest(entry.path())) continue;
                if (root) return std::nullopt;
                root = entry.path();
            }
            return root;
        }

        std::string unique_name(const std::filesystem::path& folder, const std::string& wanted)
        {
            const auto base = wanted.empty() ? std::string("Mod") : wanted;
            auto name = base;
            for (int n = 2; std::filesystem::exists(folder / utils::string::utf8_to_path(name)); ++n)
            {
                name = std::format("{} ({})", base, n);
            }
            return name;
        }

        std::filesystem::path make_staging(const std::filesystem::path& folder)
        {
            std::random_device device{};
            return folder / utils::string::utf8_to_path(std::format("{}{:08x}", staging_prefix, device()));
        }

        bool find_entry(std::vector<order_entry>& entries, const std::string& name, order_entry*& found)
        {
            for (auto& entry : entries)
            {
                if (lower(entry.name) == lower(name))
                {
                    found = &entry;
                    return true;
                }
            }
            return false;
        }
    }

    std::filesystem::path mods_folder(const std::filesystem::path& game_dir)
    {
        return game_dir / L"data" / L"mods";
    }

    std::vector<mod_info> list(const std::filesystem::path& game_dir)
    {
        std::lock_guard lock(mutex_);
        const auto folder = mods_folder(game_dir);
        std::vector<mod_info> mods;
        if (!utils::io::directory_exists(folder))
        {
            return mods;
        }

        for (const auto& entry : reconcile(folder))
        {
            mods.push_back(describe(folder, entry));
        }
        return mods;
    }

    bool set_enabled(const std::filesystem::path& game_dir, const std::string& name, const bool enabled, std::string& error)
    {
        std::lock_guard lock(mutex_);
        const auto folder = mods_folder(game_dir);
        auto entries = reconcile(folder);
        order_entry* entry = nullptr;
        if (!find_entry(entries, name, entry))
        {
            error = "That mod is no longer installed.";
            return false;
        }

        entry->enabled = enabled;
        if (!write_order(folder, entries))
        {
            error = "Could not save the load order.";
            return false;
        }
        return true;
    }

    bool set_order(const std::filesystem::path& game_dir, const std::vector<std::string>& names, std::string& error)
    {
        std::lock_guard lock(mutex_);
        const auto folder = mods_folder(game_dir);
        auto entries = reconcile(folder);

        std::vector<order_entry> reordered;
        for (const auto& name : names)
        {
            order_entry* entry = nullptr;
            if (find_entry(entries, name, entry) && std::ranges::none_of(reordered, [&](const auto& e) { return lower(e.name) == lower(name); }))
            {
                reordered.push_back(*entry);
            }
        }
        for (const auto& entry : entries) // anything the request left out keeps its relative place at the end
        {
            if (std::ranges::none_of(reordered, [&](const auto& e) { return lower(e.name) == lower(entry.name); }))
            {
                reordered.push_back(entry);
            }
        }

        if (!write_order(folder, reordered))
        {
            error = "Could not save the load order.";
            return false;
        }
        return true;
    }

    std::optional<std::string> import(const std::filesystem::path& game_dir, const std::filesystem::path& source, const bool is_zip, std::string& error)
    {
        const auto folder = mods_folder(game_dir);
        std::error_code created;
        std::filesystem::create_directories(folder, created);
        if (created)
        {
            error = "Could not create the mods folder.";
            return std::nullopt;
        }

        // Unpack or copy into a staging folder beside the mods, so the final step is a rename.
        const auto staging = make_staging(folder);
        const auto cleanup = utils::finally([&staging]
        {
            std::error_code ignored;
            std::filesystem::remove_all(staging, ignored);
        });

        if (is_zip)
        {
            error = archive::extract_zip(source, staging);
            if (!error.empty())
            {
                return std::nullopt;
            }
        }
        else
        {
            if (!utils::io::directory_exists(source))
            {
                error = "The selected folder could not be found.";
                return std::nullopt;
            }
            if (utils::io::is_inside_folder(folder, source) || utils::io::is_inside_folder(source, folder))
            {
                error = "Pick a folder outside the game's mods folder.";
                return std::nullopt;
            }
            try
            {
                // Keep the folder's own name as the wrapper, as a zip of it would have.
                std::filesystem::create_directories(staging);
                const auto wrapper = staging / source.filename();
                std::filesystem::create_directories(wrapper);
                for (const auto& entry : std::filesystem::recursive_directory_iterator(source))
                {
                    if (!parts::ordinary_path(entry.path()))
                    {
                        error = "Choose a pack without linked files or folders.";
                        return std::nullopt;
                    }
                    const auto dst = wrapper / entry.path().lexically_relative(source);
                    if (entry.is_directory()) std::filesystem::create_directory(dst);
                    else if (entry.is_regular_file()) std::filesystem::copy_file(entry.path(), dst);
                }
            }
            catch (const std::exception&)
            {
                error = "The folder could not be copied.";
                return std::nullopt;
            }
        }

        const auto root = find_content_root(staging);
        if (!root)
        {
            error = "The selected ZIP or folder must contain one content pack with a pack.json file.";
            return std::nullopt;
        }
        std::error_code fs_error;
        const auto wanted = sanitize_name(utils::string::path_to_utf8(*root == staging ?
            (is_zip ? source.stem() : source.filename()) : root->filename()));

        std::lock_guard lock(mutex_);
        const auto name = unique_name(folder, wanted);
        std::filesystem::rename(*root, folder / utils::string::utf8_to_path(name), fs_error);
        if (fs_error)
        {
            error = "The mod could not be moved into the mods folder.";
            return std::nullopt;
        }

        reconcile(folder);
        return name;
    }

    bool uninstall(const std::filesystem::path& game_dir, const std::string& name, std::string& error)
    {
        std::lock_guard lock(mutex_);
        const auto folder = mods_folder(game_dir);
        const auto target = (folder / utils::string::utf8_to_path(name)).lexically_normal();
        if (name.empty() || sanitize_name(name) != name || !parts::ordinary_path(target) ||
            !utils::io::is_inside_folder(target, folder) || !has_pack_manifest(target))
        {
            error = "That mod is no longer installed.";
            return false;
        }

        std::error_code fs_error;
        std::filesystem::remove_all(target, fs_error);
        if (fs_error)
        {
            error = "The mod's files could not all be removed. Close the game and try again.";
            return false;
        }

        reconcile(folder);
        return true;
    }
}
