#include "records.hpp"

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

        std::optional<std::uint64_t> length(HANDLE file)
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(file, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
            return (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        }

        std::string hash_file(const fs::path& file, std::uint64_t expected)
        {
            const handle stream(CreateFileW(extended(file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!stream || length(stream.get()) != expected) return {};
            hash digest;
            std::array<char, 65536> bytes{};
            std::uint64_t total{};
            for (;;)
            {
                DWORD read{};
                if (!ReadFile(stream.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return {};
                if (!read) break;
                if (!digest.append(bytes.data(), read)) return {};
                total += read;
            }
            return total == expected ? digest.finish() : std::string{};
        }

        std::string fold(std::string text)
        {
            for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            return text;
        }

        bool missing(DWORD error)
        {
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
    }

    bool plain_path(const fs::path& path, bool allow_missing_leaf)
    {
        const auto drive = path.root_name().native();
        if (!path.is_absolute() || drive.size() != 2 || drive.back() != L':' ||
            path.native().size() > 32760 || GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) return false;
        auto current = path.root_path();
        const auto relative = path.relative_path();
        for (auto at = relative.begin(); at != relative.end(); ++at)
        {
            const auto name = at->native();
            if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ' ||
                name.find_first_of(L":<>\"|?*") != name.npos) return false;
            current /= *at;
            const auto attributes = GetFileAttributesW(extended(current).c_str());
            const auto leaf = std::next(at) == relative.end();
            if (attributes == INVALID_FILE_ATTRIBUTES) return allow_missing_leaf && leaf && missing(GetLastError());
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
            if (!leaf && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        }
        return true;
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
        if (!plain_path(file)) return {};
        const handle stream(CreateFileW(extended(file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!stream) return {};
        const auto bytes = length(stream.get());
        if (!bytes || !*bytes || *bytes > limit || *bytes > MAXDWORD) return {};
        std::string result(static_cast<std::size_t>(*bytes), '\0');
        DWORD read{};
        if (!ReadFile(stream.get(), result.data(), static_cast<DWORD>(result.size()), &read, nullptr) ||
            read != result.size()) return {};
        return result;
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

    verification verify(const fs::path& folder, const check_mode mode)
    {
        const auto raw = read_record(folder / "version.txt", 2 * 1024 * 1024);
        const auto manifest = parse_inventory(raw);
        if (!manifest) return {sha256(raw), "version.txt"};
        const auto bad = [&](const std::string& file) { return verification{manifest->fingerprint, file}; };
        auto name = folder.filename().string();
        if (mode == check_mode::partial && name.ends_with(".partial")) name.resize(name.size() - 8);
        if (!valid_folder(name) || folder_version(name) != manifest->version || manifest->bootstrap > bootstrap_version)
            return bad("version.txt");
        std::map<std::string, std::string> listed;
        for (const auto& entry : manifest->files)
        {
            const auto path = folder / entry.path;
            if (!plain_path(path)) return bad(entry.path);
            std::error_code error;
            const auto size = fs::file_size(extended(path), error);
            if (error || size != entry.size) return bad(entry.path);
            if (mode != check_mode::quick && hash_file(path, entry.size) != entry.sha256) return bad(entry.path);
            listed.emplace(fold(entry.path), entry.path);
        }
        if (mode == check_mode::quick) return {manifest->fingerprint, {}};

        std::error_code error;
        const fs::path base(extended(folder));
        for (fs::recursive_directory_iterator it(base, error), end; !error && it != end; it.increment(error))
        {
            const auto relative = it->path().lexically_relative(base).generic_string();
            if (!safe_relative(relative) || !plain_path(folder / relative)) return bad(relative);
            if (it->is_directory(error)) continue;
            if (error || !it->is_regular_file(error) || error) return bad(relative);
            const auto key = fold(relative);
            if (key == "version.txt" || (mode == check_mode::partial && key == ".writing")) continue;
            if (!listed.erase(key)) return bad(relative);
        }
        if (error || !listed.empty()) return bad(listed.empty() ? "version.txt" : listed.begin()->second);
        return {manifest->fingerprint, {}};
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

    bool commit(const fs::path& root, const pointer& old, const pointer& next)
    {
        const auto before = encode(old), after = encode(next);
        if (!parse_pointer(before) || !parse_pointer(after))
        {
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }
        return write_record(root / "pointer.bak", before) && write_record(root / "pointer.txt", after);
    }
}
