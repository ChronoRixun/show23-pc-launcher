#include <std_include.hpp>

#include "cef/cef_ui_app.hpp"

#include <utils/flags.hpp>

namespace cef
{
    cef_ui_app::cef_ui_app() = default;

    void cef_ui_app::OnContextInitialized()
    {
        CEF_REQUIRE_UI_THREAD();
    }

    void cef_ui_app::OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line)
    {
        command_line->AppendSwitch("show23-subprocess");

        // Propagate -portable so child processes resolve the same data dir.
        if (utils::flags::has_flag("portable"))
        {
            command_line->AppendArgument("-portable");
        }
    }

    void cef_ui_app::OnBeforeCommandLineProcessing(const CefString& /*process_type*/,
                                                   CefRefPtr<CefCommandLine> command_line)
    {
        command_line->AppendSwitch("disable-background-networking");
        command_line->AppendSwitch("no-pings");
        command_line->AppendSwitchWithValue("host-resolver-rules", "MAP * ~NOTFOUND");
        command_line->AppendSwitch("in-process-gpu");
        command_line->AppendSwitchWithValue("default-encoding", "utf-8");
    }
}
