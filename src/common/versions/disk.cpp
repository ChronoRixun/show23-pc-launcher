#include "records.hpp"
#include "bootstrap_update_fixture.hpp"
#include "update_fixture.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <map>
#include <iterator>

#pragma comment(lib, "bcrypt.lib")

namespace versions
{
    namespace
    {
        class handle
        {
        public:
            explicit handle(HANDLE value) : value_(value) {}
            ~handle() { if (*this) CloseHandle(value_); }
            handle(const handle&) = delete;
            handle& operator=(const handle&) = delete;
            HANDLE get() const { return value_; }
            explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
        private:
            HANDLE value_;
        };

        class hash
        {
        public:
            hash()
            {
                if (BCryptOpenAlgorithmProvider(&provider_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
                    BCryptCreateHash(provider_, &hash_, nullptr, 0, nullptr, 0, 0);
            }
            ~hash()
            {
                if (hash_) BCryptDestroyHash(hash_);
                if (provider_) BCryptCloseAlgorithmProvider(provider_, 0);
            }
            hash(const hash&) = delete;
            hash& operator=(const hash&) = delete;

            bool append(const void* data, ULONG length)
            {
                return hash_ && BCryptHashData(hash_, const_cast<PUCHAR>(static_cast<const UCHAR*>(data)), length, 0) >= 0;
            }
            std::string finish()
            {
                std::array<UCHAR, 32> bytes{};
                if (!hash_ || BCryptFinishHash(hash_, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) return {};
                std::string result;
                for (const auto byte : bytes)
                {
                    constexpr std::string_view hex = "0123456789abcdef";
                    result += hex[byte >> 4];
                    result += hex[byte & 15];
                }
                return result;
            }
        private:
            BCRYPT_ALG_HANDLE provider_{};
            BCRYPT_HASH_HANDLE hash_{};
        };

        std::wstring extended(fs::path path)
        {
            return L"\\\\?\\" + path.make_preferred().native();
        }

        enum class file_status { ok, missing, invalid, unknown };

        file_status classify(DWORD error)
        {
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? file_status::missing : file_status::unknown;
        }

        file_status inspect_path(const fs::path& path, DWORD* leaf_attributes = nullptr, bool allow_missing_leaf = false)
        {
            const auto drive = path.root_name().native();
            if (!path.is_absolute() || drive.size() != 2 || drive.back() != L':' ||
                path.native().size() > 32760 || GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) return file_status::invalid;
            auto current = path.root_path();
            const auto relative = path.relative_path();
            for (auto at = relative.begin(); at != relative.end(); ++at)
            {
                const auto name = at->native();
                if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ' ||
                    name.find_first_of(L":<>\"|?*") != name.npos) return file_status::invalid;
                current /= *at;
                const auto attributes = GetFileAttributesW(extended(current).c_str());
                const auto leaf = std::next(at) == relative.end();
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const auto status = classify(GetLastError());
                    return allow_missing_leaf && leaf && status == file_status::missing ? file_status::ok : status;
                }
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return file_status::invalid;
                if (!leaf && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return file_status::invalid;
                if (leaf && leaf_attributes) *leaf_attributes = attributes;
            }
            return file_status::ok;
        }

        std::uint64_t combine(DWORD high, DWORD low) { return (std::uint64_t(high) << 32) | low; }

        struct opened_file
        {
            file_status status{file_status::ok};
            handle stream;
            BY_HANDLE_FILE_INFORMATION info{};

            opened_file(const fs::path& path, DWORD access, DWORD share, DWORD flags = 0)
                : stream(open(path, access, share, flags))
            {
                if (!stream) return;
                if (!GetFileInformationByHandle(stream.get(), &info)) status = file_status::unknown;
                else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
                    status = file_status::invalid;
            }

            std::uint64_t size() const { return combine(info.nFileSizeHigh, info.nFileSizeLow); }
            std::uint64_t last_write() const { return combine(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime); }

            std::optional<detail::file_stamp> stamp(std::string_view path) const
            {
                const auto index = combine(info.nFileIndexHigh, info.nFileIndexLow);
                FILE_BASIC_INFO basic{};
                if (!index || !GetFileInformationByHandleEx(stream.get(), FileBasicInfo, &basic, sizeof(basic))) return {};
                return detail::file_stamp{std::string(path), size(), last_write(),
                    static_cast<std::uint64_t>(basic.ChangeTime.QuadPart), info.dwVolumeSerialNumber, index};
            }

        private:
            HANDLE open(const fs::path& path, DWORD access, DWORD share, DWORD flags)
            {
                DWORD attributes{};
                status = inspect_path(path, &attributes);
                if (status != file_status::ok) return INVALID_HANDLE_VALUE;
                if (attributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    status = file_status::invalid;
                    return INVALID_HANDLE_VALUE;
                }
                const auto result = CreateFileW(extended(path).c_str(), access, share, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | flags, nullptr);
                if (result == INVALID_HANDLE_VALUE) status = classify(GetLastError());
                return result;
            }
        };

        std::string hash_file(HANDLE stream, std::uint64_t expected)
        {
            hash digest;
            std::array<char, 65536> bytes{};
            std::uint64_t total{};
            for (;;)
            {
                DWORD read{};
                if (!ReadFile(stream, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return {};
                if (!read) break;
                if (read > expected - total || !digest.append(bytes.data(), read)) return {};
                total += read;
            }
            return total == expected ? digest.finish() : std::string{};
        }

        std::string fold(std::string text)
        {
            for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            return text;
        }

        std::string read_bytes(const opened_file& file, std::size_t limit, file_status& status)
        {
            status = file.status;
            if (status != file_status::ok) return {};
            const auto bytes = file.size();
            if (!bytes || bytes > limit || bytes > MAXDWORD)
            {
                status = file_status::invalid;
                return {};
            }
            std::string result(static_cast<std::size_t>(bytes), '\0');
            DWORD read{};
            if (!ReadFile(file.stream.get(), result.data(), static_cast<DWORD>(result.size()), &read, nullptr) ||
                read != result.size())
            {
                status = file_status::unknown;
                return {};
            }
            return result;
        }
    }

    bool plain_path(const fs::path& path, bool allow_missing_leaf)
    {
        return inspect_path(path, nullptr, allow_missing_leaf) == file_status::ok;
    }

    std::string sha256(std::string_view data)
    {
        hash digest;
        while (!data.empty())
        {
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(data.size(), 65536));
            if (!digest.append(data.data(), chunk)) return {};
            data.remove_prefix(chunk);
        }
        return digest.finish();
    }

    std::string read_record(const fs::path& file, std::size_t limit)
    {
        // A reader sees one immutable snapshot and must not block an atomic replacement.
        const opened_file input(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE);
        file_status status{};
        return read_bytes(input, limit, status);
    }

    bool write_record(const fs::path& file, std::string_view bytes)
    {
        if (bytes.empty() || bytes.size() > MAXDWORD || !plain_path(file, true))
        {
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }
        auto temp = file;
        temp += L".tmp";
        if (!plain_path(temp, true))
        {
            SetLastError(ERROR_INVALID_NAME);
            return false;
        }
        DWORD error{};
        {
            const handle stream(CreateFileW(extended(temp).c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!stream) return false;
            DWORD written{};
            if (!WriteFile(stream.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
                error = GetLastError();
            else if (written != bytes.size()) error = ERROR_WRITE_FAULT;
            else if (!FlushFileBuffers(stream.get())) error = GetLastError();
        }
        if (!error)
        {
            if (MoveFileExW(extended(temp).c_str(), extended(file).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            error = GetLastError();
        }
        DeleteFileW(extended(temp).c_str());
        SetLastError(error);
        return false;
    }

    std::string request_id()
    {
        std::array<UCHAR, 8> bytes{};
        if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
        std::string result;
        for (const auto byte : bytes)
        {
            constexpr std::string_view hex = "0123456789abcdef";
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }

    namespace
    {
        std::optional<inventory> load_inventory(const opened_file& source, const fs::path& folder,
            check_mode mode, verification& result)
        {
            file_status status{};
            const auto raw = read_bytes(source, 2 * 1024 * 1024, status);
            auto manifest = parse_inventory(raw);
            if (!manifest)
            {
                auto fingerprint = status == file_status::unknown ? std::string{} : sha256(raw);
                const bool inconclusive = fingerprint.empty();
                result = {std::move(fingerprint), "version.txt", inconclusive};
                return {};
            }
            auto name = folder.filename().string();
            if (mode == check_mode::partial && name.ends_with(".partial")) name.resize(name.size() - 8);
            if (!valid_folder(name) || folder_version(name) != manifest->version || manifest->bootstrap > bootstrap_version)
            {
                result = {manifest->fingerprint, "version.txt"};
                return {};
            }
            return manifest;
        }

        verification verify_tree(const fs::path& folder, const inventory& manifest, bool partial = false)
        {
            std::map<std::string, std::string> listed;
            for (const auto& entry : manifest.files) listed.emplace(fold(entry.path), entry.path);
            std::error_code error;
            const fs::path base(extended(folder));
            for (fs::recursive_directory_iterator it(base, error), end; !error && it != end; it.increment(error))
            {
                const auto relative = it->path().lexically_relative(base).generic_string();
                if (!safe_relative(relative)) return {manifest.fingerprint, relative};
                DWORD attributes{};
                const auto status = inspect_path(folder / relative, &attributes);
                if (status != file_status::ok) return {manifest.fingerprint, relative, status == file_status::unknown};
                if (attributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const auto key = fold(relative);
                if (key == "version.txt" || (partial && key == ".writing")) continue;
                if (!listed.erase(key)) return {manifest.fingerprint, relative};
            }
            if (error || !listed.empty())
                return {manifest.fingerprint, listed.empty() ? "version.txt" : listed.begin()->second, bool(error)};
            return {manifest.fingerprint, {}};
        }

        verification verify_files(const fs::path& folder, const inventory& manifest, check_mode mode,
            std::vector<detail::file_stamp>* capture = nullptr)
        {
            const bool full = mode == check_mode::full || mode == check_mode::partial;
            if (capture) capture->reserve(manifest.files.size());
            for (const auto& entry : manifest.files)
            {
                const auto key = fold(entry.path);
                const bool startup_file = key == launcher_exe || (key.starts_with("cef/") && key.ends_with(".dll"));
                const bool check_hash = full || (mode == check_mode::startup && startup_file);
                const opened_file file(folder / entry.path, check_hash ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                    check_hash ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    check_hash ? FILE_FLAG_SEQUENTIAL_SCAN : 0);
                if (file.status != file_status::ok || file.size() != entry.size)
                    return {manifest.fingerprint, entry.path, file.status == file_status::unknown};
                if (check_hash)
                {
                    const auto digest = hash_file(file.stream.get(), entry.size);
                    if (digest != entry.sha256) return {manifest.fingerprint, entry.path, digest.empty()};
                }
                // Measure the same file handle that supplied the verified bytes.
                if (capture)
                {
                    if (auto stamp = file.stamp(entry.path)) capture->push_back(std::move(*stamp));
                    else
                    {
                        capture->clear();
                        capture = nullptr;
                    }
                }
            }
            return full ? verify_tree(folder, manifest, mode == check_mode::partial) : verification{manifest.fingerprint, {}};
        }

        std::optional<verification> verify_metadata(const fs::path& folder, const inventory& manifest,
            const std::vector<detail::file_stamp>& stamps)
        {
            if (stamps.size() != manifest.files.size()) return {};
            for (std::size_t i = 0; i < stamps.size(); ++i)
            {
                const auto& entry = manifest.files[i];
                const auto& saved = stamps[i];
                if (saved.path != entry.path || saved.size != entry.size) return {};
                // Request read access so locks and ACL failures surface even on the metadata path.
                const opened_file file(folder / entry.path, GENERIC_READ, FILE_SHARE_READ);
                if (file.status != file_status::ok)
                    return verification{manifest.fingerprint, entry.path, file.status == file_status::unknown};
                if (file.stamp(entry.path) != saved) return {};
            }
            auto result = verify_tree(folder, manifest);
            result.used_cached_metadata = static_cast<bool>(result);
            return result;
        }

        bool ntfs(const fs::path& folder)
        {
            wchar_t filesystem[MAX_PATH]{};
            return GetVolumeInformationW(folder.root_path().c_str(), nullptr, 0, nullptr, nullptr,
                nullptr, filesystem, MAX_PATH) && std::wstring_view(filesystem) == L"NTFS";
        }

        struct confirmation_read
        {
            std::optional<detail::confirmation> record;
            std::uint64_t last_write{};
            file_status status{};
        };

        confirmation_read load_confirmation(const fs::path& root, std::string_view name)
        {
            const opened_file ledger(detail::confirmation_path(root, name), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE);
            confirmation_read result;
            const auto bytes = read_bytes(ledger, detail::confirmation_limit, result.status);
            result.record = detail::parse_confirmation(bytes, name);
            result.last_write = ledger.last_write();
            return result;
        }

        std::uint64_t now()
        {
            FILETIME time{};
            GetSystemTimePreciseAsFileTime(&time);
            return combine(time.dwHighDateTime, time.dwLowDateTime);
        }
    }

    verification verify(const fs::path& folder, const check_mode mode)
    {
        const opened_file source(folder / "version.txt", GENERIC_READ, FILE_SHARE_READ);
        verification result;
        const auto manifest = load_inventory(source, folder, mode, result);
        return manifest ? verify_files(folder, *manifest, mode) : result;
    }

    verification verify_confirmed(const fs::path& root, std::string_view name, bool refresh)
    {
        if (!valid_folder(name)) return {{}, "version.txt"};
        const auto folder = root / std::string(name);
        const opened_file source(folder / "version.txt", GENERIC_READ, FILE_SHARE_READ);
        verification result;
        const auto manifest = load_inventory(source, folder, check_mode::full, result);
        if (!manifest) return result;

        const auto ledger = load_confirmation(root, name);
        const auto& record = ledger.record;
        if (ledger.status == file_status::unknown) return {manifest->fingerprint, "confirmation", true};
        if (!record || record->fingerprint != manifest->fingerprint)
            return {manifest->fingerprint, "confirmation", false, true};

        const bool cacheable = ntfs(folder);
        if (cacheable && !record->files.empty())
        {
            if (auto cached = verify_metadata(folder, *manifest, record->files)) return std::move(*cached);
        }

        const auto confirmed_at = record->confirmed_at ? record->confirmed_at : ledger.last_write;
        detail::confirmation updated{manifest->fingerprint, {}, confirmed_at};
        result = verify_files(folder, *manifest, check_mode::full, refresh && cacheable ? &updated.files : nullptr);
        // A valid prior confirmation still applies after a successful full check if the refresh cannot be written.
        if (result && refresh && (!record->confirmed_at || record->files != updated.files))
            detail::persist_confirmation(root, name, updated);
        return result;
    }

    bool capture_confirmation(const fs::path& root, std::string_view name, std::string_view expected_fingerprint)
    {
        if (!valid_folder(name))
        {
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }
        const auto folder = root / std::string(name);
        const opened_file source(folder / "version.txt", GENERIC_READ, FILE_SHARE_READ);
        verification result;
        const auto manifest = load_inventory(source, folder, check_mode::full, result);
        if (!manifest || manifest->fingerprint != expected_fingerprint)
        {
            SetLastError(result.retryable ? ERROR_READ_FAULT : ERROR_INVALID_DATA);
            return false;
        }
        detail::confirmation record{manifest->fingerprint, {}};
        result = verify_files(folder, *manifest, check_mode::full, ntfs(folder) ? &record.files : nullptr);
        if (!result)
        {
            SetLastError(result.retryable ? ERROR_READ_FAULT : ERROR_INVALID_DATA);
            return false;
        }
        record.confirmed_at = now();
        return detail::persist_confirmation(root, name, record);
    }

    std::vector<confirmation_entry> confirmation_history(const fs::path& root)
    {
        struct dated_confirmation
        {
            confirmation_entry entry;
            std::uint64_t time;
        };
        const auto directory = root / "confirmed";
        if (!plain_path(directory)) return {};
        std::vector<dated_confirmation> records;
        std::error_code error;
        for (fs::directory_iterator it(fs::path(extended(directory)), error), end; !error && it != end; it.increment(error))
        {
            const auto filename = it->path().filename().native();
            if (!filename.ends_with(L".txt") || std::ranges::any_of(filename, [](wchar_t c) { return c > 127; })) continue;
            std::string name;
            name.reserve(filename.size() - 4);
            for (const auto c : std::wstring_view(filename).substr(0, filename.size() - 4))
                name.push_back(static_cast<char>(c));
            if (!valid_folder(name)) continue;
            DWORD attributes{};
            if (inspect_path(root / name, &attributes) != file_status::ok || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const auto ledger = load_confirmation(root, name);
            if (!ledger.record) continue;
            const auto time = ledger.record->confirmed_at ? ledger.record->confirmed_at : ledger.last_write;
            records.push_back({{std::move(name), ledger.record->fingerprint}, time});
        }
        if (error) return {};
        std::ranges::sort(records, [](const auto& a, const auto& b) {
            if (a.time != b.time) return a.time > b.time;
            if (const auto order = compare(a.entry.folder, b.entry.folder)) return order > 0;
            return a.entry.folder > b.entry.folder;
        });
        std::vector<confirmation_entry> history;
        history.reserve(records.size());
        for (auto& record : records) history.push_back(std::move(record.entry));
        return history;
    }

    std::string last_confirmed(const fs::path& root)
    {
        auto history = confirmation_history(root);
        return history.empty() ? std::string{} : std::move(history.front().folder);
    }

    loaded_pointer load_pointer(const fs::path& root)
    {
        return select_pointer(parse_pointer(read_record(root / "pointer.txt")), parse_pointer(read_record(root / "pointer.bak")));
    }

    bool repair_pointer(const fs::path& root, const loaded_pointer& loaded)
    {
        if (!loaded.repair_primary && !loaded.repair_backup) return true;
        const auto bytes = encode(loaded.value);
        if (!parse_pointer(bytes)) return false;
        return write_record(root / (loaded.repair_primary ? "pointer.txt" : "pointer.bak"), bytes);
    }

    namespace
    {
        bool commit_records(const fs::path& root, const pointer& old, const pointer& next,
            [[maybe_unused]] std::string_view step = {})
        {
            const auto before = encode(old), after = encode(next);
            if (!parse_pointer(before) || !parse_pointer(after))
            {
                SetLastError(ERROR_INVALID_DATA);
                return false;
            }
            if (!write_record(root / "pointer.bak", before)) return false;
#ifdef SHOW23_UPDATE_FIXTURE
            if (!step.empty())
            {
                const auto label = std::string(step);
                fixture::kill_at(root.parent_path(), label + "-backup");
                if (fixture::setting("SHOW23_FIXTURE_FAIL_AT") == label + "-primary" && fixture::enabled(root.parent_path()))
                {
                    SetLastError(ERROR_ACCESS_DENIED);
                    return false;
                }
            }
#endif
            return write_record(root / "pointer.txt", after);
        }
    }

    bool commit(const fs::path& root, const pointer& old, const pointer& next)
    {
        return commit_records(root, old, next);
    }

#ifdef SHOW23_UPDATE_FIXTURE
    bool fixture::commit_pointer(const fs::path& root, const pointer& old, const pointer& next, std::string_view step)
    {
        return commit_records(root, old, next, step);
    }
#endif
}
