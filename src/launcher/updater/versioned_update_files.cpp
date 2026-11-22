#include "versioned_update_files.hpp"
#include <versions/update_fixture.hpp>

#include <versions/records.hpp>
#include <Windows.h>
#include <miniz.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <vector>

namespace versioned_update_files
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr std::string_view partial_suffix = ".partial";
        constexpr std::string_view download_prefix = ".download-";
        constexpr auto max_inventory = 2 * 1024 * 1024;

        bool plain_directory(const fs::path& path)
        {
            std::error_code error;
            return versions::plain_path(path) && fs::is_directory(path, error);
        }

        bool path_present(const fs::path& path)
        {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) return true;
            const auto error = GetLastError();
            return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
        }

        bool transient_name(std::string_view name)
        {
            if (name.ends_with(partial_suffix))
                return versions::valid_folder(name.substr(0, name.size() - partial_suffix.size()));
            if (!name.starts_with(download_prefix)) return false;
            name.remove_prefix(download_prefix.size());
            return !name.empty() && name.size() <= 20 && name.find_first_not_of("0123456789") == name.npos;
        }

        std::string ascii_lower(std::string text)
        {
            for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            return text;
        }

        struct occupied_slot
        {
            unsigned number;
            std::string name;
        };

        std::optional<std::vector<occupied_slot>> occupied_slots(const fs::path& root, std::string_view version)
        {
            std::vector<occupied_slot> slots;
            std::error_code error;
            for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
            {
                const auto filename = it->path().filename().u8string();
                std::string name(filename.begin(), filename.end());
                const auto folded = ascii_lower(name);
                if (versions::folder_version(folded) != version || !versions::valid_folder(folded)) continue;
                unsigned number = 1;
                if (folded != version)
                    std::from_chars(folded.data() + version.size() + 1, folded.data() + folded.size(), number);
                slots.push_back({number, std::move(name)});
            }
            if (error) return {};
            std::ranges::sort(slots, std::greater<>(), &occupied_slot::number);
            return slots;
        }

        // ZIP data is written only through this bounded stream into the caller's private partial.
        // A corrupt entry cannot exceed its recorded expansion even if the decompressor emits it.
        struct entry_sink
        {
            std::ofstream stream;
            std::uint64_t remaining;
            const std::function<bool()>& cancelled;
        };

        size_t write_entry(void* opaque, mz_uint64, const void* data, size_t size)
        {
            auto& sink = *static_cast<entry_sink*>(opaque);
            if (size > sink.remaining || sink.cancelled()) return 0;
            sink.stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            sink.remaining -= size;
            return sink.stream ? size : 0;
        }

        struct zip_archive
        {
            FILE* file{};
            mz_zip_archive zip{};
            bool opened{};

            ~zip_archive()
            {
                if (opened) mz_zip_reader_end(&zip);
                if (file) fclose(file);
            }
        };

        struct zip_entry
        {
            mz_uint index;
            std::string name;
            std::uint64_t size;
        };

        bool add_size(std::uint64_t& sum, std::uint64_t size)
        {
            if (std::numeric_limits<std::uint64_t>::max() - sum < size) return false;
            sum += size;
            return true;
        }

        // The portable archive also contains top-level launchers, scripts and documentation.
        // Leave those entries untouched. Only the exact selected version subtree can be written.
        std::string collect_entries(mz_zip_archive& zip, const std::string& prefix,
                                    std::vector<zip_entry>& files, std::uint64_t& expanded)
        {
            std::set<std::string> names;
            const auto count = mz_zip_reader_get_num_files(&zip);
            for (mz_uint index = 0; index < count; ++index)
            {
                mz_zip_archive_file_stat info{};
                if (!mz_zip_reader_file_stat(&zip, index, &info))
                    return "The launcher ZIP has a damaged entry.";
                std::string_view path(info.m_filename);
                if (!path.starts_with(prefix) || path.size() == prefix.size()) continue;
                if (mz_zip_reader_get_filename(&zip, index, nullptr, 0) != path.size() + 1)
                    return "The launcher ZIP has an invalid entry name in the version folder.";
                path.remove_prefix(prefix.size());
                if (info.m_is_directory && path.ends_with('/')) path.remove_suffix(1);
                const auto folded = ascii_lower(std::string(path));
                const auto unix_type = (info.m_external_attr >> 16) & 0170000u;
                if (!versions::safe_relative(path) || folded == ".writing" ||
                    ((info.m_version_made_by >> 8) == 3 && unix_type != 0 &&
                        unix_type != (info.m_is_directory ? 0040000u : 0100000u)) ||
                    !names.insert(folded).second)
                    return "The launcher ZIP contains an unsafe or duplicate entry in the version folder.";
                if (info.m_is_directory) continue;
                if ((folded == "version.txt" && (path != folded || info.m_uncomp_size > max_inventory)) ||
                    files.size() >= 8193 || !add_size(expanded, info.m_uncomp_size))
                    return "The launcher ZIP has an invalid file inventory or expanded size.";
                files.push_back({index, std::string(path), info.m_uncomp_size});
            }
            if (!names.contains("version.txt") || files.empty())
                return "The launcher ZIP does not contain the selected version's files.";
            return {};
        }
    }

    bool has_space(std::uint64_t available, std::uint64_t archive, std::uint64_t expanded)
    {
        if (!add_size(archive, expanded) || archive > available) return false;
        const auto margin = archive / 10 + (archive % 10 != 0);
        return available - archive >= margin;
    }

    std::optional<std::string> verified_slot(const fs::path& root, std::string_view version)
    {
        if (!versions::valid_version(version) || !plain_directory(root)) return {};
        const auto plain = root / version;
        if (plain_directory(plain) && versions::verify(plain, versions::check_mode::full)) return std::string(version);

        std::optional<std::string> result;
        std::error_code error;
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
        {
            const auto name = it->path().filename().string();
            if (name == version || versions::folder_version(name) != version || !versions::valid_folder(name) ||
                (result && (name.size() > result->size() || (name.size() == result->size() && name >= *result)))) continue;
            if (plain_directory(it->path()) && versions::verify(it->path(), versions::check_mode::full)) result = name;
        }
        return result;
    }

    request_write try_request(const fs::path& root, std::string_view running,
        const versions::request& request, std::string_view retry_fingerprint, bool defer)
    {
        const auto pointer = versions::load_pointer(root);
        const auto selected = pointer.source == versions::pointer_source::primary && pointer.value.current == running;
        if (!selected && !defer) return request_write::pending;
        if (request.trial == running || (request.reason == "retry" &&
            !std::ranges::any_of(pointer.value.failed, [&](const auto& failure)
                { return failure.folder == request.trial && failure.fingerprint == retry_fingerprint; })))
            return request_write::rejected;

        const auto record = versions::encode(request);
        if (!versions::parse_request(record)) return request_write::rejected;
        if (versions::write_record(root / "request.txt", record))
            return selected ? request_write::saved : request_write::deferred;
        const auto error = GetLastError();
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED
            ? request_write::pending : request_write::rejected;
    }

    void remove_transient(const fs::path& folder)
    {
        if (!transient_name(folder.filename().string()) || !plain_directory(folder)) return;
        const auto marker = folder / ".writing";
        if (path_present(marker))
        {
            if (!versions::plain_path(marker)) return;
            const auto handle = CreateFileW(marker.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return;
            CloseHandle(handle);
        }

        // Do not descend into anything linked. The app and bootstrap single-instance locks
        // keep another launcher from starting a download while cleanup runs.
        std::error_code error;
        for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            if (!versions::plain_path(it->path())) return;
        if (!error) fs::remove_all(folder, error);
    }

    void cleanup(const fs::path& root)
    {
        if (!plain_directory(root)) return;
        std::error_code error;
        std::vector<fs::path> abandoned;
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
            if (transient_name(it->path().filename().string())) abandoned.push_back(it->path());
        for (const auto& folder : abandoned) remove_transient(folder);
    }

    partial_slot::partial_slot(const fs::path& root, const std::string& version)
        : root_(root), folder_(root / (version + std::string(partial_suffix))), version_(version)
    {
    }

    partial_slot::~partial_slot()
    {
        if (marker_) CloseHandle(marker_);
        if (owns_folder_)
        {
            try { remove_transient(folder_); }
            catch (const std::exception&) {} // A subsequent idle cleanup can try again.
        }
    }

    std::string partial_slot::extract(const fs::path& path, const std::function<bool()>& cancelled)
    {
        if (!versions::valid_version(version_) || !versions::plain_path(path) || !plain_directory(root_))
            return "The launcher update location is invalid.";
        std::error_code error;
        const auto archive_size = fs::file_size(path, error);
        zip_archive archive;
        if (error || _wfopen_s(&archive.file, path.c_str(), L"rb") != 0 || !archive.file)
            return "Could not open the launcher ZIP.";
        archive.opened = mz_zip_reader_init_cfile(&archive.zip, archive.file, archive_size, 0);
        if (!archive.opened) return "The launcher download is not a valid ZIP.";

        const auto prefix = "show23-pc-" + version_ + "-win64-portable/show23-pc/versions/" + version_ + "/";
        std::vector<zip_entry> files;
        std::uint64_t expanded{};
        if (const auto failure = collect_entries(archive.zip, prefix, files, expanded); !failure.empty()) return failure;
        const auto available = fs::space(root_, error).available;
        // Include the downloaded ZIP which already occupies space in .download-*.
        auto original_space = available;
        if (error || !add_size(original_space, archive_size) || !has_space(original_space, archive_size, expanded))
            return "There is not enough disk space for the launcher ZIP, its extracted files and 10% extra space.";

        if (cancelled()) return "The launcher update was cancelled.";
        owns_folder_ = versions::plain_path(folder_, true) && fs::create_directory(folder_, error);
        if (!owns_folder_ || error) return "Could not create the launcher's partial version folder.";
        const auto marker = CreateFileW((folder_ / ".writing").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (marker == INVALID_HANDLE_VALUE) return "Could not lock the launcher's partial version folder.";
        marker_ = marker;

        bool first = true;
        for (const auto& file : files)
        {
            if (cancelled()) return "The launcher update was cancelled.";
            const auto target = folder_ / file.name;
            fs::create_directories(target.parent_path(), error);
            if (error || !versions::plain_path(target, true) || path_present(target))
                return "Could not create " + file.name + " in the launcher update.";
            entry_sink output{std::ofstream(target, std::ios::binary), file.size, cancelled};
            if (!output.stream || !mz_zip_reader_extract_to_callback(&archive.zip, file.index, write_entry, &output, 0) ||
                output.remaining || !output.stream.flush()) return "Could not extract " + file.name + " from the launcher ZIP.";
            if (first) versions::fixture::kill_at(root_.parent_path(), "U3-extract");
            first = false;
        }

        const auto result = versions::verify(folder_, versions::check_mode::partial);
        if (!result) return "The extracted launcher update failed verification (" + result.bad_file + ").";
        fingerprint_ = result.fingerprint;
        versions::fixture::kill_at(root_.parent_path(), "U4-verified");
        return {};
    }

    std::string partial_slot::publish(const fs::path& manifest, const versions::pointer* arrival, std::string& folder)
    {
        if (fingerprint_.empty() || !marker_) return "The launcher update has not been verified.";
        const auto occupied = occupied_slots(root_, version_);
        if (!occupied) return "Could not find a free launcher version folder.";

        std::string name;
        for (const auto& slot : *occupied)
        {
            if (!versions::valid_folder(slot.name) || !plain_directory(root_ / slot.name) ||
                (arrival && (slot.name == arrival->previous || slot.name == arrival->current))) continue;
            const auto verified = versions::verify(root_ / slot.name, versions::check_mode::full);
            if (!verified) continue;

            const bool identical = verified.fingerprint == fingerprint_;
            if (arrival && std::ranges::any_of(arrival->failed, [&](const auto& failure)
                { return failure.folder == slot.name && failure.fingerprint == verified.fingerprint; }))
            {
                if (identical) return "This launcher version failed to start. Use Try again to request another start.";
                continue;
            }
            if (identical) name = slot.name;
            // Implicit arrival tries the highest eligible same-tag slot first.
            if (identical || arrival) break;
        }

        const bool reuse = !name.empty();
        if (!reuse)
        {
            const auto highest = occupied->empty() ? 0u : occupied->front().number;
            if (highest == std::numeric_limits<unsigned>::max()) return "Could not find a free launcher version folder.";
            name = highest ? version_ + '~' + std::to_string(highest + 1) : version_;
            if (path_present(root_ / name)) return "Could not find a free launcher version folder.";
        }
        const auto receipt = root_ / (name + ".launcher.json");
        if (!versions::plain_path(receipt, true) || !versions::plain_path(manifest) ||
            !MoveFileExW(manifest.c_str(), receipt.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return "Could not save the verified release manifest.";

        if (arrival)
        {
            // This record is actionable only if the complete folder exists with this seal.
            // Write it before the rename so interruption after publication preserves priority.
            const auto publication = root_ / (name + ".arrival.txt");
            const auto temp = root_ / (name + ".arrival.txt.tmp");
            if (versions::plain_path(temp)) DeleteFileW(temp.c_str());
            if (!versions::write_record(publication, fingerprint_ + '\n'))
                return "Could not record the verified launcher arrival.";
        }

        if (!reuse)
        {
            CloseHandle(marker_);
            marker_ = nullptr;
            if (!DeleteFileW((folder_ / ".writing").c_str())) return "Could not close the partial version.";
            versions::fixture::kill_at(root_.parent_path(), "U5-before-rename");
            if (!MoveFileExW(folder_.c_str(), (root_ / name).c_str(), MOVEFILE_WRITE_THROUGH))
                return "Could not publish the verified launcher version folder.";
            owns_folder_ = false;
            versions::fixture::kill_at(root_.parent_path(), "U5-after-rename");
        }
        folder = std::move(name);
        return {};
    }
}
