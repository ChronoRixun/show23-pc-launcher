#include "std_include.hpp"
#include "input_paths.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <iterator>
#include <utility>

namespace show23
{
    namespace
    {
        fs::path relative_components(const fs::path& path)
        {
            auto relative = path.relative_path();
            if (!relative.empty() && !relative.has_filename()) relative = relative.parent_path();
            return relative;
        }

        bool safe_component(const std::wstring& name)
        {
            if (name.empty() || name.size() > 255 || name.back() == L'.' || name.back() == L' ' ||
                name.find_first_of(L"<>:\"/\\|?*") != name.npos ||
                std::ranges::any_of(name, [](wchar_t c) { return c < 32; })) return false;
            auto prefix = name.substr(0, name.find(L'.'));
            while (!prefix.empty() && prefix.back() == L' ') prefix.pop_back();
            for (auto& ch : prefix)
                if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
            if (prefix == L"CON" || prefix == L"PRN" || prefix == L"AUX" || prefix == L"NUL" ||
                prefix == L"CONIN$" || prefix == L"CONOUT$") return false;
            return !(prefix.size() == 4 && (prefix.starts_with(L"COM") || prefix.starts_with(L"LPT")) &&
                     std::wstring_view(L"0123456789\u00b9\u00b2\u00b3").find(prefix.back()) != prefix.npos);
        }
    }

    bool is_local_path(const fs::path& path)
    {
        const auto drive = path.root_name().wstring();
        if (!path.is_absolute() || drive.size() != 2 || drive[1] != L':' ||
            !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z')) ||
            path.native().size() > 32760 || GetDriveTypeW(path.root_path().c_str()) == DRIVE_REMOTE) return false;
        const auto relative = relative_components(path);
        return std::ranges::all_of(relative, [](const fs::path& component) { return safe_component(component.native()); });
    }
}

namespace show23::input_paths
{
    namespace
    {
        std::wstring extended(const fs::path& path)
        {
            auto preferred = path;
            return L"\\\\?\\" + preferred.make_preferred().native();
        }

        fs::path resolved(const HANDLE file)
        {
            const auto length = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (length < 5 || length > 32768) return {};
            std::wstring result(length, L'\0');
            const auto used = GetFinalPathNameByHandleW(file, result.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (used < 6 || used >= length) return {};
            result.resize(used);
            if (!result.starts_with(L"\\\\?\\") || result.size() < 7 || result[5] != L':') return {};
            return fs::path(result.substr(4));
        }

        bool missing(const DWORD error)
        {
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }

        bool inode(const fs::path& path, BY_HANDLE_FILE_INFORMATION& info, location& result, const bool read_data)
        {
            const auto desired = read_data ? GENERIC_READ : FILE_READ_ATTRIBUTES;
            const auto sharing = read_data ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE;
            handle pin(CreateFileW(extended(path).c_str(), desired, sharing, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (!pin)
            {
                result.error = missing(GetLastError()) ? path_error::missing : path_error::access;
                return false;
            }
            if (!GetFileInformationByHandle(pin.get(), &info))
            {
                result.error = path_error::access;
                return false;
            }
            if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                result.error = path_error::reparse;
                return false;
            }
            result.pins.push_back(std::move(pin));
            return true;
        }

        bool sum_folder(const fs::path& path, std::uint64_t& sum, unsigned depth)
        {
            if (depth == 256) return false;
            WIN32_FIND_DATAW entry{};
            const auto search = FindFirstFileW(extended(path / L"*").c_str(), &entry);
            if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
            struct finder { HANDLE value; ~finder() { FindClose(value); } } guard{search};
            do
            {
                const std::wstring_view name(entry.cFileName);
                if (name == L"." || name == L"..") continue;
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    const auto child = path / name;
                    BY_HANDLE_FILE_INFORMATION info{};
                    location pin;
                    if (!inode(child, info, pin, false) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                        !sum_folder(child, sum, depth + 1)) return false;
                }
                else
                {
                    const auto length = (static_cast<std::uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
                    if (length > (std::numeric_limits<std::uint64_t>::max)() - sum) return false;
                    sum += length;
                }
            } while (FindNextFileW(search, &entry));
            return GetLastError() == ERROR_NO_MORE_FILES;
        }
    }

    handle::handle(const HANDLE value) : value_(value) {}
    handle::~handle() { if (*this) CloseHandle(value_); }
    handle::handle(handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    handle& handle::operator=(handle&& other) noexcept
    {
        if (this != &other)
        {
            if (*this) CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    bool fixed_drive(const fs::path& path)
    {
        return is_local_path(path) && GetDriveTypeW(path.root_path().c_str()) == DRIVE_FIXED;
    }

    location inspect(const fs::path& path, const bool allow_missing, const bool read_file)
    {
        location result;
        if (!is_local_path(path))
        {
            result.error = path_error::access;
            return result;
        }
        const auto parts = relative_components(path);
        auto current = path.root_path();
        BY_HANDLE_FILE_INFORMATION info{};
        if (!inode(current, info, result, false)) return result;
        for (auto at = parts.begin(); at != parts.end(); ++at)
        {
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                result.error = path_error::not_directory;
                return result;
            }
            current /= *at;
            if (inode(current, info, result, read_file && std::next(at) == parts.end())) continue;
            if (allow_missing && result.error == path_error::missing)
            {
                result.existing = resolved(result.pins.back().get());
                if (result.existing.empty())
                {
                    result.error = path_error::access;
                    return result;
                }
                result.path = result.existing;
                for (; at != parts.end(); ++at) result.path /= *at;
                result.error = path_error::none;
            }
            return result;
        }
        result.present = true;
        result.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        result.length = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        result.path = resolved(result.pins.back().get());
        if (result.path.empty()) result.error = path_error::access;
        result.existing = result.path;
        return result;
    }

    bool contains(const fs::path& directory, const fs::path& path)
    {
        auto at = path.begin();
        for (const auto& component : directory)
        {
            if (at == path.end() ||
                CompareStringOrdinal(component.c_str(), -1, at->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
            ++at;
        }
        return !directory.empty();
    }

    bool read_at(const HANDLE file, const std::uint64_t offset, std::span<char> out)
    {
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()) ||
            out.size() > (std::numeric_limits<DWORD>::max)()) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD actual = 0;
        return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
            ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &actual, nullptr) && actual == out.size();
    }

    std::optional<std::uint64_t> folder_size(const fs::path& folder)
    {
        std::uint64_t size = 0;
        return sum_folder(folder, size, 0) ? std::optional(size) : std::nullopt;
    }

    bool probe(const fs::path& existing, const bool creating_folder)
    {
        // A missing destination requires permission to add a directory to its nearest
        // existing parent. Probe with a delete-on-close file so a crash leaves no test file.
        handle parent;
        if (creating_folder)
        {
            parent = handle(CreateFileW(extended(existing).c_str(), FILE_ADD_SUBDIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!parent) return false;
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(parent.get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return false;
        }

        static std::atomic_uint32_t serial{};
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const auto name = L".show23-write-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial);
            const auto probe_path = extended(existing / name);
            handle test(CreateFileW(probe_path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (test) return true;
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return false;
        }
        return false;
    }

    std::optional<std::uint64_t> free_space(const fs::path& existing)
    {
        ULARGE_INTEGER bytes{};
        if (!GetDiskFreeSpaceExW(extended(existing).c_str(), &bytes, nullptr, nullptr)) return std::nullopt;
        return bytes.QuadPart;
    }
}
