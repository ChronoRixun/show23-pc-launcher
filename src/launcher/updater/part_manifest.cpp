#include "std_include.hpp"
#include "part_manifest.hpp"

#include <initializer_list>
#include <sstream>
#include <unordered_set>

namespace parts
{
    namespace
    {
        using values = std::initializer_list<std::string_view>;
        constexpr std::size_t maximum_manifest = 256 * 1024;
        constexpr rapidjson::SizeType maximum_files = 2048;

        bool object_has_keys(const rapidjson::Value& object, const values keys)
        {
            if (!object.IsObject() || object.MemberCount() != keys.size()) return false;
            for (const auto key : keys)
            {
                std::size_t count = 0;
                for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member)
                {
                    if (key == std::string_view(member->name.GetString(), member->name.GetStringLength())) ++count;
                }
                if (count != 1) return false;
            }
            return true;
        }

        std::string_view field(const rapidjson::Value& value)
        {
            if (!value.IsString()) return {};
            return {value.GetString(), value.GetStringLength()};
        }

        bool valid_requirement(std::string_view text)
        {
            if (text == "*") return true;
            if (text.empty() || text.size() > 192 || text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789. -<=>") != text.npos)
                return false;
            std::istringstream tokens{std::string(text)};
            for (std::string token; tokens >> token;)
            {
                if (token.starts_with(">=") || token.starts_with("<=") || token.starts_with("=="))
                    token.erase(0, 2);
                else if (token[0] == '<' || token[0] == '>' || token[0] == '=')
                    token.erase(0, 1);
                else
                    return false;
                if (!is_version(token)) return false;
            }
            return text.find_first_not_of(' ') != text.npos;
        }

        bool valid_path(std::string_view path)
        {
            if (path.empty() || path.size() > 200 || path.front() == '/' || path.back() == '/' ||
                path.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-+/") != path.npos) return false;
            while (!path.empty())
            {
                const auto slash = path.find('/');
                const auto component = path.substr(0, slash);
                if (component.empty() || component.front() == '.' || component.back() == '.') return false;
                std::string base(component.substr(0, component.find('.')));
                for (auto& ch : base) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - ('a' - 'A'));
                if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                    (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
                     base.back() >= '0' && base.back() <= '9')) return false;
                if (slash == path.npos) return true;
                path.remove_prefix(slash + 1);
            }
            return false;
        }
    }

    const char* name(const id part)
    {
        constexpr std::array names{"launcher", "emulator", "server", "profile"};
        return names[index(part)];
    }

    const char* display_name(const id part)
    {
        return std::array{"Launcher", "Game engine", "Local server", "Profile"}[index(part)];
    }

    bool is_version(const std::string_view version)
    {
        if (version.empty() || version.size() > 48 || version.find('+') != version.npos) return false;
        const auto parsed = release_channel::parse_version(version);
        return parsed && parsed->core.size() == 3 &&
               release_channel::tag_version("v" + std::string(version)) == version;
    }

    std::optional<manifest> parse(const std::string_view json, std::string& error)
    {
        const auto invalid = [&error](const char* reason) -> std::optional<manifest>
        {
            error = reason;
            return std::nullopt;
        };
        error.clear();
        if (json.empty() || json.size() > maximum_manifest || json.find('\0') != json.npos)
            return invalid("Manifest is empty, too large or contains a null byte.");

        rapidjson::Document doc;
        doc.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(json.data(), json.size());
        if (doc.HasParseError() ||
            !object_has_keys(doc, {"schema", "part", "version", "channel", "min_launcher", "files", "compatibility"}) ||
            !doc["schema"].IsUint() || doc["schema"].GetUint() != 1)
            return invalid("Expected a schema 1 part manifest with unique, known fields.");

        const auto type = field(doc["part"]);
        const auto part = std::find_if(all.begin(), all.end(), [type](id value) { return type == name(value); });
        const auto version = field(doc["version"]);
        const auto min_launcher = field(doc["min_launcher"]);
        if (part == all.end() || !is_version(version) || !is_version(min_launcher))
            return invalid("Manifest has an unknown part or an invalid version.");

        const auto channel_text = field(doc["channel"]);
        if ((channel_text != "stable" && channel_text != "beta") ||
            (channel_text == "beta") != release_channel::is_prerelease(version))
            return invalid("Manifest channel does not match its version.");

        manifest result{*part, std::string(version), release_channel::parse_channel(channel_text),
                        std::string(min_launcher), {}, {}};
        const auto& files = doc["files"];
        if (!files.IsArray() || files.Empty() || files.Size() > maximum_files)
            return invalid("Manifest must list between 1 and 2048 files.");
        std::unordered_set<std::string> paths;
        for (const auto& item : files.GetArray())
        {
            if (!object_has_keys(item, {"path", "size", "sha256"}))
                return invalid("Manifest file must have path, size and sha256.");
            const auto path = field(item["path"]);
            const auto hash = field(item["sha256"]);
            if (!valid_path(path) || !item["size"].IsUint64() ||
                hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != hash.npos)
                return invalid("Manifest contains an unsafe path, invalid size or invalid SHA-256.");
            std::string lower(path);
            for (auto& ch : lower) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
            if (!paths.insert(lower).second) return invalid("Manifest contains a duplicate file path.");
            result.files.push_back({std::string(path), item["size"].GetUint64(), std::string(hash)});
        }
        for (const auto& path : paths)
        {
            for (auto slash = path.find('/'); slash != path.npos; slash = path.find('/', slash + 1))
                if (paths.contains(path.substr(0, slash))) return invalid("A manifest file is also a parent folder.");
        }

        const auto& compatibility = doc["compatibility"];
        if (!compatibility.IsObject() || compatibility.MemberCount() != all.size() - 1)
            return invalid("Manifest must give a compatibility rule for every other part.");
        for (const auto other : all)
        {
            if (other == result.part) continue;
            const auto rule = compatibility.FindMember(name(other));
            if (rule == compatibility.MemberEnd() || !valid_requirement(field(rule->value)))
                return invalid("Manifest contains a missing or invalid compatibility rule.");
            result.compatibility[index(other)] = field(rule->value);
        }
        return result;
    }

    std::string compatibility_error(const install_set& proposed, const std::string_view launcher_version)
    {
        for (const auto& part : proposed)
        {
            if (!part) continue;
            for (const auto other : all)
            {
                if (other == part->part) continue;
                const auto& rule = part->compatibility[index(other)];
                if (rule == "*") continue;
                const auto& item = proposed[index(other)];
                const auto version = other == id::launcher ? launcher_version :
                    (item ? std::string_view(item->version) : std::string_view{});
                if (!release_channel::satisfies(version, rule))
                    return std::format("{} {} requires {} {} ({}).", display_name(part->part), part->version, display_name(other),
                                       rule, version.empty() ? "not installed" : version);
            }
        }
        return {};
    }

    std::string install_error(const install_set& installed, const manifest& candidate,
                              const std::string_view running_launcher, const release_channel::channel selected,
                              const bool downgrade_confirmed)
    {
        if (selected == release_channel::channel::stable && candidate.channel == release_channel::channel::beta)
            return "Stable does not install pre-releases.";
        if (!is_version(running_launcher) || release_channel::compare(running_launcher, candidate.min_launcher) < 0)
            return "This release requires launcher " + candidate.min_launcher + " or newer to install it.";
        const auto& old = installed[index(candidate.part)];
        const auto previous = candidate.part == id::launcher ? running_launcher : (old ? std::string_view(old->version) : std::string_view{});
        if (!release_channel::may_install(previous, candidate.version, downgrade_confirmed))
            return "The installed version is newer. Switching back requires confirmation.";
        auto proposed = installed;
        proposed[index(candidate.part)] = candidate;
        return compatibility_error(proposed, candidate.part == id::launcher ? std::string_view(candidate.version) : running_launcher);
    }
}
