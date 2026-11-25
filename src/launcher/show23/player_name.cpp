#include "std_include.hpp"
#include "player_name.hpp"

#include <Windows.h>
#include <Lmcons.h>

#pragma comment(lib, "advapi32.lib")

namespace show23
{
    namespace
    {
        bool allowed(wchar_t ch)
        {
            return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
                (ch >= L'0' && ch <= L'9') || ch == L' ' || ch == L'-' || ch == L'_';
        }
    }

    std::optional<std::string> normalize_player_name(std::string_view name)
    {
        const auto first = name.find_first_not_of(' ');
        if (first == name.npos) return std::nullopt;
        name = name.substr(first, name.find_last_not_of(' ') - first + 1);
        if (name.size() < 3 || name.size() > 16 || !std::ranges::all_of(name, allowed)) return std::nullopt;
        return std::string(name);
    }

    std::string suggest_player_name(std::wstring_view account)
    {
        std::string result;
        for (const auto ch : account)
        {
            if (!allowed(ch) || (result.empty() && ch == L' ')) continue;
            result += static_cast<char>(ch);
            if (result.size() == 16) break;
        }
        return normalize_player_name(result).value_or("Player");
    }

    std::string windows_player_name()
    {
        wchar_t account[UNLEN + 1]{};
        DWORD size = UNLEN + 1;
        return GetUserNameW(account, &size) && size ? suggest_player_name({account, size - 1}) : "Player";
    }
}
