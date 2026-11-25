#include "std_include.hpp"
#include <WinSock2.h>
#include "play_session.hpp"
#include "child_process.hpp"
#include "card_import.hpp"

#include <iphlpapi.h>
#include <utils/string.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <format>
#include <regex>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace show23
{
    namespace
    {
        using namespace input_paths;
        using clock = std::chrono::steady_clock;

        struct winsock
        {
            WSADATA data{};
            bool ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            ~winsock() { if (ready) WSACleanup(); }
        };

        struct socket_handle
        {
            SOCKET value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ~socket_handle() { if (value != INVALID_SOCKET) closesocket(value); }
        };

        sockaddr_in endpoint(std::uint16_t port)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            return address;
        }

        bool available(std::uint16_t port)
        {
            const socket_handle s;
            if (s.value == INVALID_SOCKET) return false;
            const auto address = endpoint(port);
            BOOL exclusive = TRUE;
            return setsockopt(s.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                              reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0 &&
                bind(s.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
        }

        bool connected(std::uint16_t port)
        {
            const socket_handle s;
            if (s.value == INVALID_SOCKET) return false;
            const auto address = endpoint(port);
            u_long nonblocking = 1;
            if (ioctlsocket(s.value, FIONBIO, &nonblocking) != 0) return false;
            if (connect(s.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return true;
            if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
            WSAPOLLFD status{s.value, POLLWRNORM, 0};
            int error{};
            int size = sizeof(error);
            return WSAPoll(&status, 1, 50) == 1 &&
                getsockopt(s.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 && !error;
        }

        bool owns_listeners(DWORD pid, const std::array<std::uint16_t, 2>& ports)
        {
            DWORD bytes{};
            if (GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
                return false;
            // The table can grow while obtaining it.
            for (int retry = 0; retry < 3 && bytes <= 8 * 1024 * 1024; ++retry)
            {
                std::vector<std::byte> buffer(bytes);
                const auto status = GetExtendedTcpTable(buffer.data(), &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
                if (status == ERROR_INSUFFICIENT_BUFFER) continue;
                if (status != NO_ERROR) return false;
                const auto& table = *reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
                const auto rows = std::span(table.table, table.dwNumEntries);
                return std::ranges::all_of(ports, [&](auto port)
                {
                    return std::ranges::any_of(rows, [&](const auto& row)
                    {
                        return row.dwOwningPid == pid && row.dwLocalAddr == htonl(INADDR_LOOPBACK) &&
                            ntohs(static_cast<u_short>(row.dwLocalPort)) == port;
                    });
                });
            }
            return false;
        }

        std::string log_suffix()
        {
            SYSTEMTIME now{};
            GetSystemTime(&now);
            static std::atomic_uint sequence{};
            return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}-{}-{}.log", now.wYear, now.wMonth, now.wDay,
                now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), ++sequence);
        }

        handle open_log(const fs::path& path, SECURITY_ATTRIBUTES& inherit)
        {
            return handle(CreateFileW(extended(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        }

        std::string tail(const fs::path& path)
        {
            const auto file = inspect(path, false, true);
            if (!file.present || !file.length || file.error != path_error::none || file.directory) return {};
            std::string text(static_cast<std::size_t>((std::min)(file.length, std::uint64_t{32 * 1024})), '\0');
            if (!read_at(file.pins.back().get(), file.length - text.size(), text)) return {};
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            std::size_t lines{};
            for (std::size_t pos = text.size(); pos > 0; --pos)
                if (text[pos - 1] == '\n' && ++lines == 20) { text.erase(0, pos); break; }
            std::erase_if(text, [](unsigned char c) { return c < 32 && c != '\n' && c != '\r' && c != '\t'; });
            if (text.empty()) return {};
            // Normalize even if the child used a different encoding or the read began mid-character.
            const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (!size) return {};
            std::wstring wide(size, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
            return utils::string::convert(wide);
        }

        bool plain_file(const location& file)
        {
            return file.present && !file.directory && file.error == path_error::none;
        }

        bool seed_config(const fs::path& package, const location& profile, const fs::path& name)
        {
            const auto current = inspect(profile.path / name, true);
            if (current.error != path_error::none || current.directory) return false;
            if (current.present) return current.length != 0;
            const auto source = inspect(package / name, false, true);
            if (!plain_file(source) || !source.length || source.length > 1024 * 1024) return false;
            std::string bytes(static_cast<std::size_t>(source.length), '\0');
            return read_at(source.pins.back().get(), 0, bytes) && atomically_write(profile, name, bytes);
        }
    }

    bool local_server_ports_available()
    {
        const winsock network;
        return network.ready && available(10000) && available(10080);
    }

    const char* phase_name(play_phase phase)
    {
        return std::array{"idle", "checking", "preparing", "certificates", "server", "playing", "stopping"}[static_cast<std::size_t>(phase)];
    }

    play_result run_play(const play_request& request, const play_options& options)
    {
        activity_lock lock;
        if (!lock) return {"importOccupied"};
        const auto stopped = [&] { return options.stop && options.stop(); };
        const auto phase = [&](play_phase next) { if (options.phase) options.phase(next); };
        const auto profile = inspect(request.install / "data/profiles/default");
        const auto server_data = inspect(request.install / "data/profiles/default/server");
        const auto logs = inspect(request.install / "data/logs");
        const auto exe = inspect(request.engine / "bb-probe.exe", false, true);
        const auto preparer = inspect(request.engine / "prepare.py", false, true);
        const auto python = inspect(request.python, false, true);
        const auto script = inspect(request.server, false, true);
        const auto source = inspect(request.base / "eboot.bin", false, true);
        if (!profile.directory || !server_data.directory || !logs.directory ||
            !plain_file(exe) || !plain_file(preparer) || !plain_file(python) || !plain_file(script) || !plain_file(source))
            return {"paths"};

        const winsock network;
        if (!network.ready) return {"network"};
        for (const auto port : options.ports)
            if (!available(port)) return {"port", port};
        if (stopped()) return {"stopped"};

        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        const handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!input) return {"logs"};
        const auto suffix = log_suffix();
        const auto package = exe.path.parent_path();
        const auto engine_profile = ensure_directory(profile.path / "engine");
        if (engine_profile.error != path_error::none || !engine_profile.directory ||
            !seed_config(package, engine_profile, "bbport.ini") || !seed_config(package, engine_profile, "input.ini"))
            return {"engineProfile"};
        const auto user = ensure_directory(engine_profile.path / "user");
        if (user.error != path_error::none || !user.directory) return {"engineProfile"};

        phase(play_phase::preparing);
        if (stopped()) return {"stopped"};
        const auto prepare_log = logs.path / ("prepare-" + suffix);
        auto result_path = prepare_log;
        result_path.replace_extension(".txt");
        child_process preparation;
        {
            const auto output = open_log(prepare_log, inherit);
            const std::array args{std::wstring(L"-B"), std::wstring(L"-X"), std::wstring(L"utf8"),
                std::wstring(L"-u"), preparer.path.native(), request.install.native(),
                std::wstring(L"--launcher-result"), result_path.native()};
            if (!output || !preparation.start(python.path, package, args, input.get(), output.get(), true))
                return {"prepareLaunch"};
        }
        while (preparation.running() && !stopped()) Sleep(30);
        const auto preparation_code = preparation.exit_code();
        preparation.stop();
        if (stopped()) return {"stopped"};
        if (!preparation_code || *preparation_code) return {"prepareExit", preparation_code.value_or(0), tail(prepare_log)};
        const auto result = inspect(result_path, false, true);
        if (!plain_file(result) || !result.length || result.length > 128) return {"prepareOutput", 0, tail(prepare_log)};
        std::string cache_key(static_cast<std::size_t>(result.length), '\0');
        static const std::regex cache_name(R"(v[0-9]+-[0-9a-f]{64}-[0-9a-f]{16})");
        if (!read_at(result.pins.back().get(), 0, cache_key) || !std::regex_match(cache_key, cache_name))
            return {"prepareOutput", 0, tail(prepare_log)};
        const auto cache = request.install / "data/engine-cache/CUSA35521" / cache_key;
        const auto boot = inspect(cache / "boot-linked.bin", false, true);
        const auto content = inspect(cache / "content.bin", false, true);
        if (!plain_file(boot) || !boot.length || !plain_file(content) || !content.length)
            return {"prepareOutput", 0, tail(prepare_log)};
        if (stopped()) return {"stopped"};

        const auto run_server = [&](child_process& child, const wchar_t* mode, const fs::path& log)
        {
            const auto output = open_log(log, inherit);
            const std::array args{script.path.native(), std::wstring(mode), std::wstring(L"--install"), request.install.native()};
            return output && child.start(python.path, script.path.parent_path(), args, input.get(), output.get(), true);
        };
        const auto ca = server_data.path / "ca.pem";
        bool has_ca{};
        {
            const auto initial_ca = inspect(ca, true);
            if (initial_ca.error != path_error::none || initial_ca.directory) return {"certificate"};
            has_ca = initial_ca.present;
        }
        if (!has_ca)
        {
            phase(play_phase::certificates);
            const auto log = logs.path / ("server-certs-" + suffix);
            child_process generator;
            if (!run_server(generator, L"gen-certs", log)) return {"certLaunch"};
            const auto end = clock::now() + options.ready_timeout;
            while (generator.running() && !stopped() && clock::now() < end) Sleep(30);
            const auto code = generator.exit_code();
            generator.stop();
            if (stopped()) return {"stopped"};
            if (!code) return {"certTimeout", 0, tail(log)};
            if (*code) return {"certExit", *code, tail(log)};
            const auto generated = inspect(ca, false, true);
            if (!plain_file(generated) || !generated.length) return {"certificate", 0, tail(log)};
        }

        phase(play_phase::server);
        const auto server_log = logs.path / ("server-console-" + suffix);
        child_process server;
        if (!run_server(server, L"run", server_log)) return {"serverLaunch"};
        const auto deadline = clock::now() + options.ready_timeout;
        while (!stopped() && server.running() && clock::now() < deadline)
        {
            if (owns_listeners(server.id(), options.ports) &&
                std::ranges::all_of(options.ports, connected)) break;
            Sleep(100);
        }
        if (stopped()) { server.stop(); return {"stopped"}; }
        if (!server.running())
        {
            const auto code = server.exit_code().value_or(0);
            server.stop();
            return {"serverExit", code, tail(server_log)};
        }
        if (clock::now() >= deadline)
        {
            server.stop();
            return {"serverTimeout", 0, tail(server_log)};
        }

        const auto certificate = inspect(ca, false, true);
        if (!plain_file(certificate) || !certificate.length)
        {
            server.stop();
            return {"certificate", 0, tail(server_log)};
        }
        if (stopped()) { server.stop(); return {"stopped"}; }

        const auto engine_log = logs.path / ("engine-" + suffix);
        const std::array args{boot.path.native(), std::wstring(L"--content-profile"), content.path.native(),
            std::wstring(L"--app0"), source.path.parent_path().native(), std::wstring(L"--user"), user.path.native(),
            std::wstring(L"--timeout"), std::wstring(L"0"), std::wstring(L"--show23-local-ca"), certificate.path.native()};
        const environment_entry environment[] = {
            {L"BB_CONFIG", (engine_profile.path / "bbport.ini").native()},
            {L"BB_INPUT_CONFIG", (engine_profile.path / "input.ini").native()},
            {L"BB_GPU_USER_DIR", user.path.native()},
            {L"BB_VBLANK_HZ", L"0"},
            {L"BB_FPS_LIMIT", L"120"},
            {L"BB_PRESENT_MODE", L"Mailbox"},
            {L"BB_READBACK_SMALL_BUFFERS", L"1"},
            {L"BB_READBACK_SMALL_GATE", L"1"},
            {L"BB_READBACK_SMALL_MAX", L"4096"},
            {L"BB_READBACK_TARGETS", L"256x512"},
            {L"BB_UPNP", L"0"},
            {L"BB_LOG_BUFFER", L"1"},
            {L"BB_WINDOW_SIZE", std::format(L"{}x{}", request.display_resolution.width, request.display_resolution.height)},
            {L"BB_FULLSCREEN", request.fullscreen ? L"1" : L"0"},
            {L"BB_USER_NAME", request.player_name.empty() ? L"Player" : utils::string::convert(request.player_name)},
            {L"TEMP", logs.path.native()},
            {L"TMP", logs.path.native()},
        };
        child_process engine;
        {
            const auto output = open_log(engine_log, inherit);
            if (!output || !engine.start(exe.path, package, args, input.get(), output.get(), false, environment, L"BB_"))
            {
                server.stop();
                return {"engineLaunch"};
            }
        }
        phase(play_phase::playing);
        while (engine.running() && server.running() && !stopped()) Sleep(50);
        const auto engine_running = engine.running();
        const auto code = engine.exit_code();
        const auto server_code = server.exit_code().value_or(0);
        phase(play_phase::stopping);
        engine.stop();
        server.stop();
        if (stopped()) return {"stopped"};
        if (engine_running && !code) return {"serverExit", server_code, tail(server_log)};
        if (!code) return {"engineLaunch"};
        if (!*code) return {"finished"};
        return {"engineExit", *code, tail(engine_log)};
    }
}
