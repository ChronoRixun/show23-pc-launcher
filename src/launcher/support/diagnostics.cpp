#include "std_include.hpp"
#include "diagnostics.hpp"
#include "report_rules.hpp"
#include "updater/launcher_update.hpp"
#include "updater/part_store.hpp"
#include "updater/release_source.hpp"

#include <utils/flags.hpp>
#include <utils/nt.hpp>
#include <utils/properties.hpp>
#include <utils/string.hpp>

namespace support
{
    namespace
    {
        display_mode read_mode(const DWORD which)
        {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            // nullptr: the display device of the primary monitor.
            if (!EnumDisplaySettingsW(nullptr, which, &dm)) return {};
            return {static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight),
                    dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0};
        }

        int primary_monitor_dpi()
        {
            const utils::nt::library shcore{"shcore.dll"};
            if (const auto get_dpi = shcore ? shcore.get_proc<HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*)>("GetDpiForMonitor") : nullptr)
            {
                UINT dpi_x = 0, dpi_y = 0;
                const auto monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
                if (SUCCEEDED(get_dpi(monitor, 0 /*MDT_EFFECTIVE_DPI*/, &dpi_x, &dpi_y))) return static_cast<int>(dpi_x);
            }
            return 0;
        }

        int system_dpi()
        {
            const utils::nt::library user32{"user32.dll"};
            if (const auto get = user32 ? user32.get_proc<UINT(WINAPI*)()>("GetDpiForSystem") : nullptr) return static_cast<int>(get());
            return 0;
        }

        struct remembered_display
        {
            std::mutex mutex;
            display_mode mode;
            int dpi{};
        };

        remembered_display& remembered()
        {
            static remembered_display value;
            return value;
        }

        display_facts read_display()
        {
            display_facts facts;
            facts.desktop = read_mode(ENUM_REGISTRY_SETTINGS);
            facts.current = read_mode(ENUM_CURRENT_SETTINGS);
            facts.monitor_dpi = primary_monitor_dpi();
            facts.system_dpi = system_dpi();
            auto& memory = remembered();
            std::lock_guard lock(memory.mutex);
            // The desktop mode is in use now: what the monitor says is right; keep it for later reads.
            if (facts.desktop.valid() && facts.current.same_size(facts.desktop) && facts.monitor_dpi > 0)
            {
                memory.mode = facts.desktop;
                memory.dpi = facts.monitor_dpi;
            }
            facts.remembered_mode = memory.mode;
            facts.remembered_dpi = memory.dpi;
            return facts;
        }

    }

    void remember_display()
    {
        read_display();
    }

    void write_diagnostics(rapidjson::Document& out)
    {
        const auto display = choose_display(read_display());
        const auto runtime = parts::installed();
        const auto version = launcher_update::running_version();
        const auto channel = release_channel::channel_name(release_source::current_channel());
        const auto portable = utils::properties::is_portable();
        const auto offline = utils::flags::has_flag("offline");
        auto report = std::format("show23-pc launcher {} ({})\nMode: {}\nOffline: {}\nMLB The Show 23: {}\n"
                                  "Primary display: {} x {} at {} Hz ({}%)\n",
                                  version, channel, portable ? "portable" : "installed", offline ? "yes" : "no",
                                  parts::ready(runtime) ? "ready" : "not set up", display.desktop.width,
                                  display.desktop.height, display.desktop.refresh, display.scale);
        for (const auto part : parts::all)
        {
            if (part == parts::id::launcher) continue;
            const auto& item = runtime.manifests[parts::index(part)];
            report += std::format("{}: {}\n", parts::name(part), item ? item->version : "not installed");
        }
        out.SetObject();
        auto& allocator = out.GetAllocator();
        out.AddMember("text", rapidjson::Value(report.data(), static_cast<rapidjson::SizeType>(report.size()), allocator), allocator);
    }

    bool copy_text(const std::string& text_utf8, void* owner_window)
    {
        const auto wide = utils::string::convert(text_utf8);
        if (!OpenClipboard(static_cast<HWND>(owner_window))) return false;
        bool ok = false;
        if (EmptyClipboard())
        {
            const auto bytes = (wide.size() + 1) * sizeof(wchar_t);
            if (auto* const memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
            {
                if (auto* const target = GlobalLock(memory))
                {
                    std::memcpy(target, wide.c_str(), bytes);
                    GlobalUnlock(memory);
                    ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
                }
                if (!ok) GlobalFree(memory);
            }
        }
        CloseClipboard();
        return ok;
    }
}
