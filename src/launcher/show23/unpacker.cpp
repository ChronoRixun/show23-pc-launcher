#include "std_include.hpp"
#include "unpacker.hpp"
#include "child_process.hpp"

#include <utils/string.hpp>

#include <array>
#include <chrono>
#include <set>

namespace show23
{
    namespace
    {
        using namespace input_paths;
        using clock = std::chrono::steady_clock;

        constexpr auto orbis = L"orbis-unpkg.exe";
        constexpr auto pkg_tool = L"ps4-pkg-tool.exe";

        bool name_is(const fs::path& path, const wchar_t* name)
        {
            return CompareStringOrdinal(path.filename().c_str(), -1, name, -1, TRUE) == CSTR_EQUAL;
        }

        std::set<fs::path> entries(const fs::path& parent)
        {
            std::set<fs::path> result;
            for (const auto& entry : fs::directory_iterator(extended(parent)))
                result.insert(entry.path().filename());
            return result;
        }

        bool has_game_files(const fs::path& folder)
        {
            const auto boot = inspect(folder / "eboot.bin");
            const auto sfo = inspect(folder / "sce_sys" / "param.sfo");
            return boot.present && !boot.directory && boot.error == path_error::none &&
                sfo.present && !sfo.directory && sfo.error == path_error::none;
        }

        fs::path result_folder(const fs::path& output, bool allow_subfolder)
        {
            if (!folder_size(output)) return {};
            if (has_game_files(output)) return output;
            if (!allow_subfolder) return {};
            fs::path match;
            for (const auto& name : entries(output))
            {
                const auto folder = inspect(output / name);
                if (folder.error != path_error::none || !folder.directory || !contains(output, folder.path) ||
                    !has_game_files(folder.path)) continue;
                if (!match.empty()) return {};
                match = folder.path;
            }
            return match;
        }


    }

    unpacker_program find_unpacker(const fs::path& selected, const fs::path& bundled)
    {
        unpacker_program program;
        auto path = selected;
        if (path.empty() && !bundled.empty())
        {
            for (const auto* name : {orbis, pkg_tool})
            {
                const auto file = inspect(bundled / name);
                if (file.present && !file.directory && file.error == path_error::none)
                {
                    path = bundled / name;
                    program.bundled = true;
                    break;
                }
            }
        }
        if (path.empty()) program.error = "unpack.noProgram";
        else if (!name_is(path, orbis) && !name_is(path, pkg_tool)) program.error = "unpack.unsupported";
        else
        {
            program.subfolder = name_is(path, pkg_tool);
            program.file = inspect(path, false, true);
            if (!program.file.present || program.file.directory || program.file.error != path_error::none)
                program.error = "unpack.programMissing";
            else if (!program.bundled)
            {
                const auto digest = sha256(program.file);
                if (!digest) program.error = "unpack.programMissing";
                else program.consent = *digest + ":" + utils::string::path_to_utf8(program.file.path);
            }
        }
        return program;
    }

    extraction_result extract_package(const unpacker_program& program, const fs::path& package,
        const fs::path& output, const fs::path& log, const std::function<bool()>& cancel,
        const std::function<void(std::uint64_t, std::uint64_t)>& progress)
    {
        if (cancel && cancel()) return {{}, "unpack.canceled"};
        const auto input = inspect(package, false, true);
        const auto dest = inspect(output);
        const auto log_dir = inspect(log.parent_path());
        if (!input.present || input.directory || input.error != path_error::none || !dest.directory ||
            dest.error != path_error::none || !log_dir.directory || log_dir.error != path_error::none)
            return {{}, "unpack.launch"};
        const auto stage = dest.path.parent_path();
        const auto parent = stage.parent_path();
        const auto before_stage = entries(stage);
        const auto before_parent = entries(parent);

        const auto logfile = handle(CreateFileW(extended(log_dir.path / log.filename()).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!logfile) return {{}, "unpack.launch"};

        SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
        HANDLE pipe_read{}, pipe_write{};
        if (!CreatePipe(&pipe_read, &pipe_write, &inherited, 0)) return {{}, "unpack.launch"};
        handle reader(pipe_read), writer(pipe_write);
        const handle no_input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!no_input) return {{}, "unpack.launch"};
        std::vector<std::wstring> args;
        if (!program.subfolder) args.emplace_back(L"extract");
        args.push_back(input.path.native());
        if (!program.subfolder) args.emplace_back(L"-o");
        args.push_back(dest.path.native());
        child_process child;
        if (!child.start(program.file.path, dest.path, args, no_input.get(), writer.get(), true))
            return {{}, "unpack.launch"};
        writer = {};

        constexpr DWORD log_limit = 2 * 1024 * 1024;
        DWORD captured{};
        const auto drain = [&]
        {
            std::array<char, 8192> buffer{};
            // Return to cancellation and progress polling even if output never stops.
            for (unsigned block = 0; block < 128; ++block)
            {
                DWORD pending{}, count{};
                if (!PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &pending, nullptr) || !pending ||
                    !ReadFile(reader.get(), buffer.data(), (std::min)(pending, DWORD(buffer.size())), &count, nullptr))
                    break;
                if (captured < log_limit)
                {
                    DWORD ignored{};
                    WriteFile(logfile.get(), buffer.data(), (std::min)(count, log_limit - captured), &ignored, nullptr);
                    captured += (std::min)(count, log_limit - captured);
                }
            }
        };

        const auto began = clock::now();
        auto next = began + std::chrono::seconds(3);
        std::uint64_t written{}, announced{};
        bool canceled = false;
        while (true)
        {
            drain();
            if (cancel && cancel()) { canceled = true; break; }
            if (!child.running()) break;
            Sleep(50);
            const auto now = clock::now();
            if (now >= next)
            {
                written = folder_size(dest.path).value_or(written);
                next = clock::now() + std::chrono::seconds(3);
            }
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(clock::now() - began).count());
            if (progress && elapsed > announced)
            {
                announced = elapsed;
                progress(elapsed, written);
            }
        }
        const auto code = child.exit_code();
        if (!child.stop()) return {{}, "unpack.launch"};
        drain();
        if (canceled) return {{}, "unpack.canceled"};
        if (!code || *code) return {{}, "unpack.exit", code.value_or(0)};
        if (entries(stage) != before_stage || entries(parent) != before_parent) return {{}, "unpack.outside"};
        const auto result = result_folder(dest.path, true);
        if (result.empty()) return {{}, "unpack.result"};
        if (progress) progress(std::chrono::duration_cast<std::chrono::seconds>(clock::now() - began).count(),
                               folder_size(dest.path).value_or(0));
        return {result};
    }
}
