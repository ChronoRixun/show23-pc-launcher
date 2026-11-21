#include "std_include.hpp"
#include "support_commands.hpp"
#include "cef/cef_ui.hpp"

#include "support/diagnostics.hpp"

namespace commands::support_commands
{
    namespace
    {
        void set_success(rapidjson::Document& response, const bool success)
        {
            response.SetObject();
            response.AddMember("success", success, response.GetAllocator());
        }
    }

    void register_commands(cef::cef_ui& cef_ui, command_context&)
    {
        support::remember_display();

        // Support > Copy diagnostics: local facts only, redacted (support/diagnostics.hpp).
        cef_ui.add_command("get-support-diagnostics", [](const rapidjson::Value&, rapidjson::Document& response)
        {
            support::write_diagnostics(response);
        });

        // { text } -> { success }: plain text for the clipboard (the diagnostics report), at most 64 KiB.
        cef_ui.add_command("copy-to-clipboard", [&cef_ui](const rapidjson::Value& value, rapidjson::Document& response)
        {
            const bool valid = value.IsObject() && value.HasMember("text") && value["text"].IsString() && value["text"].GetStringLength() <= 65536;
            set_success(response, valid && support::copy_text(std::string(value["text"].GetString(), value["text"].GetStringLength()), cef_ui.get_window()));
        });
    }
}
