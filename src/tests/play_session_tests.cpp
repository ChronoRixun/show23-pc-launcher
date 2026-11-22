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
            request.emulator = root / "emulator directory" / "fake-emulator.exe";
            request.fullscreen = true;
            fs::create_directories(request.emulator.parent_path());
            fs::create_directories(request.install / "data/logs");
            fs::create_directories(request.install / "data/profiles/default/server");
            write(request.base / "eboot.bin", "fixture");
            write(request.server, "fixture");
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) throw std::runtime_error("Could not locate test executable");
            fs::copy_file(path, request.python);
            fs::copy_file(path, request.emulator);
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

        void modes(std::string_view server, std::string_view emulator)
        {
            const auto p1 = std::to_string(options.ports[0]), p2 = std::to_string(options.ports[1]);
            write(request.install / "test-modes", std::string(server) + "\n" + std::string(emulator) + "\n" + p1 + "\n" + p2);
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
    const bool server = image.filename() == "fake-server.exe";
    const bool emulator = image.filename() == "fake-emulator.exe";
    if (!server && !emulator) return std::nullopt;
    if ((server && args.size() != 5) || (emulator && args.size() != 8)) return 90;

    const auto cwd = fs::current_path();
    const auto install = server ? fs::path(args[4]) : cwd.parent_path().parent_path().parent_path();
    const auto profile = install / "data/profiles/default";
    const auto base = install.parent_path() / "base path";
    const auto expected_ca = profile / "server/ca.pem";
    const auto is = [&](std::size_t n, std::wstring_view expected) { return std::wstring_view(args[n]) == expected; };
    if (server ? (!is(3, L"--install") || !fs::equivalent(cwd, image.parent_path()) ||
                   !fs::equivalent(fs::path(args[1]), cwd / "olserver.py")) :
                 (!is(1, L"-g") || !is(3, L"-i") || !is(4, L"-f") || !is(5, L"true") || !is(6, L"--show23-local-ca") ||
                   !fs::equivalent(fs::path(args[2]), base / "eboot.bin") ||
                   !fs::equivalent(fs::path(args[7]), expected_ca) || !fs::equivalent(cwd, profile))) return 91;
    std::string server_mode, emulator_mode;
    std::uint16_t port_a{}, port_b{};
    if (!(std::ifstream(install / "test-modes") >> server_mode >> emulator_mode >> port_a >> port_b)) return 92;
    if (emulator)
    {
        write(profile / "emulator-pid", std::to_string(GetCurrentProcessId()));
        if (emulator_mode == "hang") Sleep(INFINITE);
        if (emulator_mode == "bad")
        {
            for (int line = 1; line <= 25; ++line) std::printf("diagnostic line %02d\n", line);
            std::fflush(stdout);
            std::fputs("Wrong executable\n", stderr);
            return 12;
        }
        return 0;
    }
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
    CHECK(f.terminated("emulator-pid"));
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
    CHECK(!f.pid("server-pid") && !f.pid("emulator-pid"));
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
    CHECK(!f.pid("emulator-pid"));
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
    CHECK(!f.pid("emulator-pid"));
}

TEST(play_includes_only_the_last_twenty_log_lines_for_a_fast_failure)
{
    play_fixture f;
    f.modes("listen", "bad");
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("emulatorExit") && result.number == 12);
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
    f.options.stop = [&] { return f.pid("emulator-pid") != 0 || GetTickCount64() >= limit; };
    const auto result = run_play(f.request, f.options);
    CHECK(result.message == std::string_view("stopped"));
    CHECK(f.terminated("server-pid") && f.terminated("emulator-pid"));
}


TEST(play_blocks_before_starting_server_on_malformed_resolution_config)
{
    play_fixture fixture;
    fixture.modes("listen", "success");
    const auto config = fixture.profile() / "user/config.json";
    write(config, "{malformed");
    const auto result = run_play(fixture.request, fixture.options);
    CHECK(result.message == std::string_view("resolutionMalformed"));
    CHECK(!fs::exists(fixture.ca()));
    CHECK(!fixture.pid("server-pid"));
    CHECK(!fixture.pid("emulator-pid"));
    std::string source;
    std::ifstream(config) >> source;
    CHECK(source == "{malformed");
}
