#pragma once

#include "records.hpp"

#ifdef SHOW23_UPDATE_FIXTURE
#include <Windows.h>
#include <shellapi.h>
#include <charconv>
#include <cwchar>
#endif

namespace versions::fixture
{
#ifdef SHOW23_UPDATE_FIXTURE
    static inline std::string setting(const char* key)
    {
        std::string value(32768, '\0');
        const auto length = GetEnvironmentVariableA(key, value.data(), static_cast<DWORD>(value.size()));
        if (!length || length >= value.size()) return {};
        value.resize(length);
        return value;
    }

    static inline bool enabled(const fs::path& data)
    {
        const auto port = setting("SHOW23_FIXTURE_PORT");
        unsigned number{};
        const auto converted = std::from_chars(port.data(), port.data() + port.size(), number);
        if (converted.ec != std::errc{} || converted.ptr != port.data() + port.size() ||
            !number || number > 65535 || !valid_version(read_record(data / "fixture-mode", 64))) return false;
        int count{};
        const auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return false;
        bool found{};
        for (int i = 1; i < count; ++i)
            if (std::wcscmp(args[i], L"--update-fixture") == 0) { found = true; break; }
        LocalFree(args);
        return found;
    }

    static inline void kill_at(const fs::path& data, std::string_view label)
    {
        if (setting("SHOW23_FIXTURE_KILL_AT") == label && enabled(data))
            TerminateProcess(GetCurrentProcess(), 111);
    }
#else
    static inline bool enabled(const fs::path&) { return false; }
    static inline void kill_at(const fs::path&, std::string_view) {}
#endif
}
