#include "std_include.hpp"
#include "card_import.hpp"
#include "child_process.hpp"
#include "play_session.hpp"
#include "server_paths.hpp"

#include <utils/named_mutex.hpp>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>

namespace show23
{
    namespace
    {
        using namespace input_paths;

        bool number(std::string_view text, unsigned& out)
        {
            if (text.empty()) return false;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
            return error == std::errc{} && end == text.data() + text.size();
        }

        bool date_valid(std::string_view date)
        {
            unsigned year{}, month{}, day{};
            if (date.size() != 10 || date[4] != '-' || date[7] != '-' ||
                !number(date.substr(0, 4), year) || !number(date.substr(5, 2), month) ||
                !number(date.substr(8), day)) return false;
            return std::chrono::year_month_day{std::chrono::year(int(year)), std::chrono::month(month),
                                                std::chrono::day(day)}.ok() && year > 0;
        }

        utils::named_mutex& activity_mutex()
        {
            static utils::named_mutex mutex("show23-pc-local-server-activity");
            return mutex;
        }

        // Windows mutexes are recursive on one thread. Prevent accidental reentry as well.
        std::atomic_bool activity_started{};

        class output_lines
        {
        public:
            void append(std::string_view bytes, const std::function<void(const card_progress&)>& report)
            {
                for (const char ch : bytes)
                {
                    if (ch == '\n') { finish(report); continue; }
                    if (line_.size() < 512 && !discard_) line_ += ch;
                    else discard_ = true;
                }
            }

            void finish(const std::function<void(const card_progress&)>& report)
            {
                if (!discard_ && parse_card_progress(line_, progress_) && report) report(progress_);
                line_.clear();
                discard_ = false;
            }

            bool completed() const { return progress_.done; }

        private:
            std::string line_;
            bool discard_{};
            card_progress progress_;
        };
    }

    activity_lock::activity_lock()
    {
        if (activity_started.exchange(true)) return;
        try { held_ = activity_mutex().try_lock(); }
        catch (...) {}
        if (!held_) activity_started = false;
    }

    activity_lock::~activity_lock()
    {
        if (!held_) return;
        activity_mutex().unlock();
        activity_started = false;
    }

    bool parse_card_progress(std::string_view line, card_progress& progress)
    {
        if (line.ends_with('\r')) line.remove_suffix(1);
        constexpr std::string_view page_prefix = "import: page ";
        constexpr std::string_view done_prefix = "import: done ";
        constexpr std::string_view done_suffix = " cards";
        if (line.starts_with(page_prefix))
        {
            line.remove_prefix(page_prefix.size());
            const auto slash = line.find('/');
            unsigned page{}, total{};
            if (slash == line.npos || !number(line.substr(0, slash), page) ||
                !number(line.substr(slash + 1), total) || !page || page > total) return false;
            progress.page = page;
            progress.total = total;
            progress.done = false;
            return true;
        }
        if (line.starts_with(done_prefix) && line.ends_with(done_suffix) &&
            line.size() > done_prefix.size() + done_suffix.size())
        {
            unsigned cards{};
            if (!number(line.substr(done_prefix.size(), line.size() - done_prefix.size() - done_suffix.size()), cards))
                return false;
            progress.cards = cards;
            progress.done = true;
            return true;
        }
        return false;
    }

    std::optional<card_status> read_card_status(const fs::path& install)
    {
        const auto cache = inspect(install / "data/import-cache");
        if (cache.error != path_error::none || !cache.directory) return std::nullopt;
        std::optional<card_status> newest;
        std::error_code error;
        for (fs::directory_iterator entry(extended(cache.path), error), end; !error && entry != end; entry.increment(error))
        {
            const auto folder = entry->path().filename().wstring();
            if (folder.size() != 10 || folder.find_first_not_of(L"0123456789-") != folder.npos) continue;
            std::string date;
            for (const auto ch : folder) date += static_cast<char>(ch);
            if (!date_valid(date) || (newest && newest->date >= date)) continue;
            const auto file = inspect(cache.path / folder / "manifest.json", false, true);
            if (file.error != path_error::none || file.directory || !file.length || file.length > 64 * 1024) continue;
            std::string bytes(static_cast<std::size_t>(file.length), '\0');
            if (!read_at(file.pins.back().get(), 0, bytes) || bytes.find('\0') != bytes.npos) continue;

            rapidjson::Document doc;
            doc.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(bytes.data(), bytes.size());
            if (doc.HasParseError() || !doc.IsObject()) continue;
            const auto complete = doc.FindMember("complete"), count = doc.FindMember("cards"), stamp = doc.FindMember("date");
            if (complete == doc.MemberEnd() || !complete->value.IsTrue() || count == doc.MemberEnd() ||
                !count->value.IsUint() || stamp == doc.MemberEnd() || !stamp->value.IsString() ||
                std::string_view(stamp->value.GetString(), stamp->value.GetStringLength()) != date) continue;
            newest = card_status{count->value.GetUint(), date};
        }
        return newest;
    }

    card_import_result import_cards(const fs::path& install, const fs::path& server_folder,
        const std::function<bool()>& cancel, const std::function<void(const card_progress&)>& report)
    {
        activity_lock lock;
        if (!lock) return {"occupied"};
        if (!local_server_ports_available()) return {"serverRunning"};
        const auto requested_cancel = [&] { return cancel && cancel(); };
        if (requested_cancel()) return {"canceled"};
        const auto paths = server_paths(server_folder);
        const auto python = inspect(paths.python, false, true);
        const auto script = inspect(paths.script, false, true);
        const auto target = inspect(install);
        if (python.error != path_error::none || python.directory || script.error != path_error::none ||
            script.directory || target.error != path_error::none || !target.directory) return {"runtime"};
        const auto logs = ensure_directory(target.path / "data/logs");
        if (logs.error != path_error::none || !logs.directory) return {"logUnavailable"};

        static std::atomic_uint sequence{};
        const auto log = logs.path / ("card-import-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence) + ".log");
        const handle logfile(CreateFileW(extended(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!logfile) return {"logUnavailable"};
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read{}, write{};
        if (!CreatePipe(&read, &write, &inherit, 0)) return {"failed", log};
        handle reader(read), writer(write);
        const handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        const std::array args{std::wstring(L"-u"), script.path.native(), std::wstring(L"import-cards"),
            std::wstring(L"--install"), target.path.native(), std::wstring(L"--pictures")};
        child_process child;
        if (!input || !child.start(python.path, script.path.parent_path(), args, input.get(), writer.get(), true))
            return {"failed", log};
        writer = {};

        output_lines lines;
        DWORD logged{};
        constexpr DWORD limit = 2 * 1024 * 1024;
        const auto drain = [&]
        {
            std::array<char, 4096> bytes{};
            for (unsigned block = 0; block < 32; ++block)
            {
                DWORD pending{}, count{};
                if (!PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &pending, nullptr) || !pending ||
                    !ReadFile(reader.get(), bytes.data(), (std::min)(pending, DWORD(bytes.size())), &count, nullptr) || !count)
                    break;
                if (logged < limit)
                {
                    DWORD written{};
                    if (WriteFile(logfile.get(), bytes.data(), (std::min)(count, limit - logged), &written, nullptr))
                        logged += written;
                }
                lines.append({bytes.data(), count}, report);
            }
        };

        while (!requested_cancel() && child.running())
        {
            drain();
            Sleep(35);
        }
        const bool canceled = requested_cancel();
        const auto code = child.exit_code();
        if (!child.stop()) return {"failed", log};
        drain();
        lines.finish(report);
        if (canceled) return {"canceled", log};
        if (code == 2) return {"network", log};
        if (code != 0 || !lines.completed() || !read_card_status(target.path)) return {"failed", log};
        return {"complete", log};
    }
}
