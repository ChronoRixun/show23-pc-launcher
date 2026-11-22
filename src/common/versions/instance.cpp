#include "instance.hpp"
#include <Windows.h>
#include <chrono>

namespace versions
{
    using namespace std::chrono_literals;

    namespace
    {
        HWND find_running_window()
        {
            HWND result{};
            EnumWindows([](HWND window, LPARAM context) -> BOOL
            {
                // The browser handler puts this property on the launcher window and its
                // children. Match the root only, regardless of the executable's folder.
                if (GetPropA(window, "show23_root_window") != window ||
                    GetPropA(window, "show23_closing") || !IsWindowVisible(window)) return TRUE;
                *reinterpret_cast<HWND*>(context) = window;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&result));
            return result;
        }
    }

    bool activate_running_window()
    {
        const auto deadline = std::chrono::steady_clock::now() + 800ms;
        do
        {
            if (const auto window = find_running_window())
            {
                if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
                else
                {
                    SetForegroundWindow(window);
                    const auto foreground = GetForegroundWindow();
                    if (foreground && GetAncestor(foreground, GA_ROOTOWNER) == window) return true;
                }
            }
            Sleep(40);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
}
