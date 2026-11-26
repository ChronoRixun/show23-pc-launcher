#include "std_include.hpp"
#include "profile_settings.hpp"
#include "input_paths.hpp"

#include <rapidjson/document.h>

namespace show23
{
    namespace
    {
        constexpr auto settings_name = "unlock-all-cards.json";

        fs::path server_profile(const fs::path& install)
        {
            return install / "data/profiles/default/server";
        }
    }

    bool profile_settings_available(const fs::path& install)
    {
        const auto folder = input_paths::inspect(server_profile(install));
        return folder.directory && folder.error == input_paths::path_error::none;
    }

    bool read_unlock_all_cards(const fs::path& install)
    {
        const auto file = input_paths::inspect(server_profile(install) / settings_name, false, true);
        if (!file.present || file.directory || file.error != input_paths::path_error::none ||
            !file.length || file.length > 4096) return false;
        std::string content(static_cast<std::size_t>(file.length), '\0');
        if (!input_paths::read_at(file.pins.back().get(), 0, content)) return false;
        rapidjson::Document document;
        document.Parse(content.data(), content.size());
        if (document.HasParseError() || !document.IsObject() || document.MemberCount() != 1) return false;
        const auto setting = document.FindMember("unlock_all_cards");
        return setting != document.MemberEnd() && setting->value.IsBool() && setting->value.GetBool();
    }

    bool write_unlock_all_cards(const fs::path& install, const bool enabled)
    {
        return input_paths::atomically_write(input_paths::inspect(server_profile(install)), settings_name,
            enabled ? "{\"unlock_all_cards\":true}\n" : "{\"unlock_all_cards\":false}\n");
    }
}
