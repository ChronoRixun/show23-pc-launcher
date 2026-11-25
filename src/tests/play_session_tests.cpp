#include "std_include.hpp"
#include <WinSock2.h>

#include "play_session_tests.hpp"
#include "show23/play_session.hpp"
#include "test.hpp"

#include <fstream>

#pragma comment(lib, "ws2_32.lib")

namespace
{
    using namespace show23;

    struct local_listener
    {
        SOCKET socket_ = INVALID_SOCKET;
        std::uint16_t port{};

        bool open(std::uint16_t desired = 0)
        {
            socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(desired);
            int size = sizeof(address);
            if (socket_ == INVALID_SOCKET || bind(socket_, reinterpret_cast<const sockaddr*>(&address), size) != 0 ||
                listen(socket_, SOMAXCONN) != 0 ||
                getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return false;
            port = ntohs(address.sin_port);
            return true;
        }

        ~local_listener() { if (socket_ != INVALID_SOCKET) closesocket(socket_); }
    };

    void write(const fs::path& path, std::string_view text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        if (!stream.write(text.data(), static_cast<std::streamsize>(text.size())))
            throw std::runtime_error("Could not write play test fixture");
    }

    const auto cache_key = "v1-" + std::string(64, 'a') + "-" + std::string(16, 'b');

    fs::path fixture_cache(const fs::path& install)
    {
        return install / "data/engine-cache/CUSA35521" / cache_key;
    }

    std::wstring environment(const wchar_t* name)
    {
        const auto size = GetEnvironmentVariableW(name, nullptr, 0);
        if (!size) return {};
        std::wstring value(size, L'\0');
        value.resize(GetEnvironmentVariableW(name, value.data(), size));
        return value;
    }

    struct play_fixture
    {
        fs::path root;
        play_request request;
        play_options options;
        WSADATA network{};

        play_fixture()
        {
            static unsigned serial{};
            root = fs::current_path() / "build" / ("play fixture " + std::to_string(GetCurrentProcessId()) +
                " " + std::to_string(GetTickCount64()) + " " + std::to_string(++serial));
            request.install = root / "install path";
            request.base = root / "base path";
            request.server = root / "server directory" / "olserver.py";
            request.python = request.server.parent_path() / "fake-server.exe";
            request.engine = root / "engine directory";
            request.player_name = "Local Batter";
            request.display_resolution = *find_resolution("720p");
            request.fullscreen = true;
            write(request.engine / "prepare.py", "fixture");
            write(request.engine / "bbport.ini", "default config");
            write(request.engine / "input.ini", "default input");
            fs::create_directories(request.install / "data/logs");
            fs::create_directories(request.install / "data/profiles/default/server");
            write(request.base / "eboot.bin", "fixture");
            write(request.server, "fixture");
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) throw std::runtime_error("Could not locate test executable");
            fs::copy_file(path, request.python);
            fs::copy_file(path, request.engine / "bb-probe.exe");
            if (WSAStartup(MAKEWORD(2, 2), &network)) throw std::runtime_error("Could not initialize fixture sockets");
            const auto choose_ports = [&]
            {
                local_listener first, second;
                if (!first.open() || !second.open()) throw std::runtime_error("Could not reserve fixture ports");
                return std::array{first.port, second.port};
            };
            options.ports = choose_ports();
            options.ready_timeout = std::chrono::seconds(3);
        }

        ~play_fixture()
        {
            WSACleanup();
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }

        void modes(std::string_view server, std::string_view engine, std::string_view prepare = "success")
        {
            const auto p1 = std::to_string(options.ports[0]), p2 = std::to_string(options.ports[1]);
            write(request.install / "test-modes", std::string(server) + "\n" + std::string(engine) + "\n" + p1 + "\n" + p2 + "\n" + std::string(prepare));
        }

        fs::path profile() const { return request.install / "data/profiles/default"; }
        fs::path ca() const { return profile() / "server/ca.pem"; }

        DWORD pid(const char* name) const
        {
            DWORD value{};
            std::ifstream(profile() / name) >> value;
            return value;
        }

        bool terminated(const char* name) const
        {
            const DWORD value = pid(name);
            if (!value) return false;
            const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, value);
            if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
            const auto dead = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
            CloseHandle(process);
            return dead;
        }
    };
}

std::optional<int> fake_play_child(const std::span<wchar_t*> args)
{
    const auto image = fs::path(args.front());
    const bool python = image.filename() == "fake-server.exe";
    const bool engine = image.filename() == "bb-probe.exe";
    if (!python && !engine) return std::nullopt;
    const bool prepare = python && args.size() == 9;
    if ((python && !prepare && args.size() != 5) || (engine && args.size() != 12)) return 90;

    const auto cwd = fs::current_path();
    const auto install = python ? fs::path(args[prepare ? 6 : 4]) : image.parent_path().parent_path() / "install path";
    const auto profile = install / "data/profiles/default";
    const auto native = profile / "engine";
    const auto base = install.parent_path() / "base path";
    const auto cache = fixture_cache(install);
    const auto expected_ca = profile / "server/ca.pem";
    const auto is = [&](std::size_t n, std::wstring_view expected) { return std::wstring_view(args[n]) == expected; };
    std::string server_mode, engine_mode, prepare_mode;
    std::uint16_t port_a{}, port_b{};
    if (!(std::ifstream(install / "test-modes") >> server_mode >> engine_mode >> port_a >> port_b >> prepare_mode)) return 92;

    if (prepare)
    {
        if (!is(1, L"-B") || !is(2, L"-X") || !is(3, L"utf8") || !is(4, L"-u") ||
            !is(7, L"--launcher-result") || !fs::equivalent(fs::path(args[5]), cwd / "prepare.py") ||
            !fs::equivalent(cwd, install.parent_path() / "engine directory")) return 91;
        write(profile / "prepare-pid", std::to_string(GetCurrentProcessId()));
        if (prepare_mode == "hang") Sleep(INFINITE);
        if (prepare_mode == "bad") { std::fputs("Fixture preparation failed\n", stderr); return 19; }
        write(cache / "boot-linked.bin", "prepared boot");
        write(cache / "content.bin", "prepared content");
        write(args[8], prepare_mode == "invalid" ? "..\\outside" : cache_key);
        return 0;
    }
    if (engine)
    {
        if (!is(2, L"--content-profile") || !is(4, L"--app0") || !is(6, L"--user") || !is(8, L"--timeout") ||
            !is(9, L"0") || !is(10, L"--show23-local-ca") || !fs::equivalent(cwd, image.parent_path()) ||
            !fs::equivalent(fs::path(args[1]), cache / "boot-linked.bin") ||
            !fs::equivalent(fs::path(args[3]), cache / "content.bin") || !fs::equivalent(fs::path(args[5]), base) ||
            !fs::equivalent(fs::path(args[7]), native / "user") || !fs::equivalent(fs::path(args[11]), expected_ca))
            return 91;
        if (environment(L"SystemDrive").empty() || environment(L"SystemRoot").empty() ||
            environment(L"BB_FULLSCREEN") != (engine_mode == "windowed" ? L"0" : L"1") ||
            environment(L"BB_WINDOW_SIZE") != L"1280x720" || environment(L"BB_USER_NAME") != L"Local Batter" ||
            environment(L"BB_PRESENT_MODE") != L"Mailbox" || environment(L"BB_READBACK_TARGETS") != L"256x512" ||
            fs::path(environment(L"BB_CONFIG")) != native / "bbport.ini" ||
            fs::path(environment(L"BB_INPUT_CONFIG")) != native / "input.ini" ||
            fs::path(environment(L"BB_GPU_USER_DIR")) != native / "user") return 96;
        write(profile / "engine-pid", std::to_string(GetCurrentProcessId()));
        if (engine_mode == "hang") Sleep(INFINITE);
        if (engine_mode == "bad")
        {
            for (int line = 1; line <= 25; ++line) std::printf("diagnostic line %02d\n", line);
            std::fflush(stdout);
            std::fputs("Wrong executable\n", stderr);
            return 12;
        }
        return 0;
    }
    if (!is(3, L"--install") || !fs::equivalent(cwd, image.parent_path()) ||
        !fs::equivalent(fs::path(args[1]), cwd / "olserver.py")) return 91;
    if (is(2, L"gen-certs"))
    {
        if (server_mode == "no-cert") { std::fputs("Fixture could not create certificate\n", stderr); return 11; }
        write(expected_ca, "fixture certificate");
        return 0;
    }
    if (!is(2, L"run")) return 93;
    write(profile / "server-pid", std::to_string(GetCurrentProcessId()));
    if (server_mode == "exit") { std::fputs("Fixture startup failed\n", stderr); return 17; }
    if (server_mode == "hang") Sleep(INFINITE);
    WSADATA network{};
    if (WSAStartup(MAKEWORD(2, 2), &network)) return 94;
    local_listener first, second;
    if (!first.open(port_a) || !second.open(port_b)) return 95;
    Sleep(INFINITE);
    return 0;
}

TEST(play_generates_the_ca_uses_exact_arguments_and_stops_server_after_success)
{
    play_fixture f;
    f.modes("listen", "success");
    std::vector<play_phase> phases;
    f.options.phase = [&](auto phase) { phases.push_back(phase); };
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("finished"));
    CHECK(fs::exists(f.ca()));
    CHECK(f.terminated("server-pid"));
    CHECK(f.terminated("engine-pid"));
    CHECK(f.terminated("prepare-pid"));
    CHECK(std::ranges::find(phases, play_phase::preparing) != phases.end());
    CHECK(std::ranges::find(phases, play_phase::certificates) != phases.end());
    CHECK(std::ranges::find(phases, play_phase::playing) != phases.end());
}

TEST(play_reports_certificate_generation_failure_with_diagnostics)
{
    play_fixture f;
    f.modes("no-cert", "success");
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("certExit") && result.number == 11);
    CHECK(result.log.find("could not create certificate") != std::string::npos);
    CHECK(!f.pid("server-pid") && !f.pid("engine-pid"));
}

TEST(play_refuses_a_named_port_before_starting_children)
{
    play_fixture f;
    local_listener other;
    CHECK(other.open(f.options.ports[1]));
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("port") && result.number == f.options.ports[1]);
    CHECK(!fs::exists(f.ca()) && !f.pid("server-pid"));
}

TEST(play_handles_early_server_exit_and_preserves_output)
{
    play_fixture f;
    f.modes("exit", "success");
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("serverExit") && result.number == 17);
    CHECK(result.log.find("startup failed") != std::string::npos);
    CHECK(!f.pid("engine-pid"));
}

TEST(play_times_out_and_stops_an_unready_server)
{
    play_fixture f;
    f.modes("hang", "success");
    write(f.ca(), "existing certificate");
    f.options.ready_timeout = std::chrono::milliseconds(600);
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("serverTimeout"));
    CHECK(f.terminated("server-pid"));
    CHECK(!f.pid("engine-pid"));
}

TEST(play_includes_only_the_last_twenty_log_lines_for_a_fast_failure)
{
    play_fixture f;
    f.modes("listen", "bad");
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("engineExit") && result.number == 12);
    CHECK(result.log.find("Wrong executable") != std::string::npos);
    CHECK(result.log.find("diagnostic line 07") != std::string::npos);
    CHECK(result.log.find("diagnostic line 06") == std::string::npos);
    CHECK(f.terminated("server-pid"));
}

TEST(play_stop_ends_both_live_processes)
{
    play_fixture f;
    f.modes("listen", "hang");
    const auto limit = GetTickCount64() + 5000;
    f.options.stop = [&] { return f.pid("engine-pid") != 0 || GetTickCount64() >= limit; };
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("stopped"));
    CHECK(f.terminated("server-pid") && f.terminated("engine-pid"));
}


TEST(play_preserves_legacy_files_and_existing_engine_settings)
{
    play_fixture f;
    f.request.fullscreen = false;
    f.modes("listen", "windowed");
    const auto config = f.profile() / "user/config.json";
    const auto users = f.profile() / "user/users.json";
    const auto save = f.profile() / "user/1000/savedata/save";
    const auto engine_config = f.profile() / "engine/bbport.ini";
    write(config, "{malformed");
    write(users, "existing-users");
    write(save, "old-save");
    write(engine_config, "custom-config");
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("finished"));
    for (const auto& [path, expected] : {std::pair{config, "{malformed"}, std::pair{users, "existing-users"},
                                       std::pair{save, "old-save"}, std::pair{engine_config, "custom-config"},
                                       std::pair{f.profile() / "engine/input.ini", "default"}})
    {
        std::string value;
        std::ifstream(path) >> value;
        CHECK(value == expected);
    }
}

TEST(play_reports_preparation_errors_without_starting_the_server)
{
    for (const auto* mode : {"bad", "invalid"})
    {
        play_fixture f;
        f.modes("listen", "success", mode);
        const auto result = run_play(f.request, f.options);
        if (std::string_view(mode) == "bad")
        {
            CHECK(result.message == std::string_view("prepareExit") && result.number == 19);
            CHECK(result.log.find("Fixture preparation failed") != std::string::npos);
        }
        else CHECK(result.message == std::string_view("prepareOutput"));
        CHECK(!f.pid("server-pid") && !fs::exists(f.ca()));
    }
}

TEST(play_stop_terminates_preparation_without_starting_the_server)
{
    play_fixture f;
    f.modes("listen", "success", "hang");
    const auto limit = GetTickCount64() + 5000;
    f.options.stop = [&] { return f.pid("prepare-pid") != 0 || GetTickCount64() >= limit; };
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("stopped"));
    CHECK(f.terminated("prepare-pid"));
    CHECK(!f.pid("server-pid") && !fs::exists(f.ca()));
}
