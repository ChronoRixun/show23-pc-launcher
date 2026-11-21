#include "std_include.hpp"
#include "player_name.hpp"
#include "input_paths.hpp"

#include <Lmcons.h>
#include <rapidjson/prettywriter.h>

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

        const rapidjson::Value* field(const rapidjson::Value& object, std::string_view key)
        {
            if (!object.IsObject()) return nullptr;
            const rapidjson::Value* value{};
            for (auto item = object.MemberBegin(); item != object.MemberEnd(); ++item)
            {
                const auto& member = *item;
                if (std::string_view(member.name.GetString(), member.name.GetStringLength()) != key) continue;
                if (value) return nullptr;
                value = &member.value;
            }
            return value;
        }

        std::string new_profile(std::string_view name)
        {
            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> out(buffer);
            out.SetIndent(' ', 2);
            out.StartObject();
            out.Key("Users"); out.StartObject();
            out.Key("commit_hash"); out.String("");
            out.Key("user"); out.StartArray();
            for (int slot = 1; slot <= 4; ++slot)
            {
                const auto display = slot == 1 ? std::string(name) : "Player " + std::to_string(slot);
                out.StartObject();
                out.Key("np_age"); out.Int(30);
                out.Key("np_country"); out.String("us");
                out.Key("np_date_of_birth"); out.String("1994-01-01");
                out.Key("np_language"); out.String("en");
                out.Key("player_index"); out.Int(slot);
                out.Key("shadnet_email"); out.String("");
                out.Key("shadnet_enabled"); out.Bool(false);
                out.Key("shadnet_npid"); out.String("");
                out.Key("shadnet_password"); out.String("");
                out.Key("shadnet_token"); out.String("");
                out.Key("user_color"); out.Int(slot);
                out.Key("user_id"); out.Int(999 + slot);
                out.Key("user_name"); out.String(display.c_str());
                out.EndObject();
            }
            out.EndArray();
            out.EndObject();
            out.EndObject();
            return buffer.GetString();
        }

        // In-situ decoding gives the source offset for the selected string. Replace its
        // original JSON token only, leaving spacing, escapes and all other fields intact.
        bool replace_name(std::string& text, std::string_view name)
        {
            if (text.find('\0') != text.npos) return false;
            auto parsed = text;
            rapidjson::Document doc;
            doc.ParseInsitu<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
                parsed.data() + (text.starts_with("\xef\xbb\xbf") ? 3 : 0));
            if (doc.HasParseError()) return false;
            const auto* users = field(doc, "Users");
            const auto* list = users ? field(*users, "user") : nullptr;
            if (!list || !list->IsArray()) return false;

            const rapidjson::Value* primary{};
            for (const auto& entry : list->GetArray())
            {
                const auto* id = field(entry, "user_id");
                if (!id || !id->IsInt()) return false;
                if (id->GetInt() != 1000) continue;
                if (primary) return false;
                primary = field(entry, "user_name");
                const auto* player = field(entry, "player_index");
                if (!primary || !primary->IsString() || !player || !player->IsInt() || player->GetInt() != 1)
                    return false;
            }
            if (!primary) return false;
            if (std::string_view(primary->GetString(), primary->GetStringLength()) == name) return true;

            const auto start = static_cast<std::size_t>(primary->GetString() - parsed.data());
            if (!start || start >= text.size() || text[start - 1] != '"') return false;
            auto end = start;
            for (; end < text.size() && text[end] != '"'; ++end)
                if (text[end] == '\\') ++end;
            if (end >= text.size()) return false;
            // The validated alphabet needs no JSON escaping.
            text.replace(start, end - start, name);
            return true;
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

    player_name_result write_player_name(const fs::path& profile, std::string_view name)
    {
        using enum player_name_result;
        using namespace input_paths;
        const auto normalized = normalize_player_name(name);
        if (!normalized) return unavailable;
        const auto directory = ensure_directory(profile / "user");
        if (!directory.directory || directory.error != path_error::none) return unavailable;
        const fs::path filename = "users.json";
        std::string contents;
        {
            const auto file = inspect(directory.path / filename, true, true);
            if (file.error != path_error::none || file.directory || (file.present && file.links != 1))
                return unavailable;
            if (!file.present) contents = new_profile(*normalized);
            else
            {
                if (!file.length || file.length > 1024 * 1024) return malformed;
                contents.resize(static_cast<std::size_t>(file.length));
                if (!read_at(file.pins.back().get(), 0, contents)) return unavailable;
                const auto original = contents;
                if (!replace_name(contents, *normalized)) return malformed;
                if (contents == original) return updated;
            }
        }
        return atomically_write(directory, filename, contents) ? updated : unavailable;
    }
}
