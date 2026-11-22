#pragma once

#include <versions/records.hpp>
#include <Windows.h>

namespace bootstrap
{
    class handle
    {
    public:
        explicit handle(HANDLE value = nullptr) : value_(value) {}
        ~handle() { if (*this) CloseHandle(value_); }
        handle(const handle&) = delete;
        handle& operator=(const handle&) = delete;
        explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
        HANDLE get() const { return value_; }
        void reset(HANDLE next) { if (*this) CloseHandle(value_); value_ = next; }
    private:
        HANDLE value_;
    };

    std::wstring widen(std::string_view text);
    std::string narrow(std::wstring_view text);
    std::wstring quote(std::wstring_view arg);

    struct arguments
    {
        bool prepare{}, manual{}, update_restart{};
        std::vector<std::wstring> flags;
    };

    arguments parse_arguments();

    struct child
    {
        handle process, confirm, job;
        DWORD launch(const versions::fs::path& exe, const versions::fs::path& bootstrap,
            const std::string& folder, bool trial, const std::vector<std::wstring>& flags);
        // Call after the bootstrap has durably promoted the confirmed trial.
        bool release_trial_kill_on_close();
    };
}
